/*
 * Copyright (C) 2017 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "super.h"
#include "format.h"
#include "kvec.h"
#include "seg.h"
#include "bio.h"
#include "cmp.h"
#include "compact.h"
#include "manifest.h"
#include "counters.h"
#include "alloc.h"
#include "net.h"
#include "scoutfs_trace.h"

/*
 * Compaction is what maintains the exponentially increasing number of
 * segments in each level of the lsm tree and is what merges duplicate
 * and deletion keys.
 *
 * When the manifest is modified in a way that requires compaction it
 * kicks the compaction thread.  The compaction thread calls into the
 * manifest to find the segments that need to be compaction.
 *
 * The compaction operation itself always involves a single "upper"
 * segment at a given level and a limited number of "lower" segments at
 * the next higher level whose key range intersects with the upper
 * segment.
 *
 * Compaction proceeds by iterating over the items in the upper segment
 * and items in each of the lower segments in sort order.  The items
 * from the two input segments are copied into new output segments in
 * sorted order.  Item space is reclaimed as duplicate or deletion items
 * are removed.
 *
 * Once the compaction is completed the manifest is updated to remove
 * the input segments and add the output segments.  Here segment space
 * is reclaimed when the input items fit in fewer output segments.
 */

struct compact_info {
	struct super_block *sb;
	struct workqueue_struct *workq;
	struct work_struct work;
};

#define DECLARE_COMPACT_INFO(sb, name) \
	struct compact_info *name = SCOUTFS_SB(sb)->compact_info

struct compact_seg {
	struct list_head entry;

	u64 segno;
	u64 seq;
	u8 level;
	struct scoutfs_key_buf *first;
	struct scoutfs_key_buf *last;
	struct scoutfs_segment *seg;
	int off;
	bool part_of_move;
};

/*
 * A compaction request.  It's filled up in scoutfs_compact_add() as
 * the manifest is wlaked and it finds segments involved in the compaction.
 */
struct compact_cursor {
	struct list_head csegs;

	/* buffer holds allocations and our returning them */
	u64 segnos[SCOUTFS_COMPACTION_MAX_UPDATE];
	unsigned nr_segnos;

	u8 lower_level;
	u8 last_level;

	struct compact_seg *upper;
	struct compact_seg *lower;

	bool sticky;
	struct compact_seg *last_lower;

	__le32 *links[SCOUTFS_MAX_SKIP_LINKS];
};

static void free_cseg(struct super_block *sb, struct compact_seg *cseg)
{
	WARN_ON_ONCE(!list_empty(&cseg->entry));

	scoutfs_seg_put(cseg->seg);
	scoutfs_key_free(sb, cseg->first);
	scoutfs_key_free(sb, cseg->last);

	kfree(cseg);
}

static struct compact_seg *alloc_cseg(struct super_block *sb,
				      struct scoutfs_key_buf *first,
				      struct scoutfs_key_buf *last)
{
	struct compact_seg *cseg;

	cseg = kzalloc(sizeof(struct compact_seg), GFP_NOFS);
	if (cseg) {
		INIT_LIST_HEAD(&cseg->entry);
		cseg->first = scoutfs_key_dup(sb, first);
		cseg->last = scoutfs_key_dup(sb, last);
		if (!cseg->first || !cseg->last) {
			free_cseg(sb, cseg);
			cseg = NULL;
		}
	}

	return cseg;
}

static void free_cseg_list(struct super_block *sb, struct list_head *list)
{
	struct compact_seg *cseg;
	struct compact_seg *tmp;

	list_for_each_entry_safe(cseg, tmp, list, entry) {
		list_del_init(&cseg->entry);
		free_cseg(sb, cseg);
	}
}

static int read_segment(struct super_block *sb, struct compact_seg *cseg)
{
	struct scoutfs_segment *seg;
	int ret;

	if (cseg == NULL || cseg->seg)
		return 0;

	seg = scoutfs_seg_submit_read(sb, cseg->segno);
	if (IS_ERR(seg)) {
		ret = PTR_ERR(seg);
	} else {
		cseg->seg = seg;
		scoutfs_inc_counter(sb, compact_segment_read);
		ret = scoutfs_seg_wait(sb, cseg->seg);
	}

	/* XXX verify read segment metadata */

	return ret;
}

static struct compact_seg *next_spos(struct compact_cursor *curs,
				     struct compact_seg *cseg)
{
	if (cseg->entry.next == &curs->csegs)
		return NULL;

	return list_next_entry(cseg, entry);
}

/*
 * Point the caller's key and value kvecs at the next item that should
 * be copied from the upper or lower segments.  We use the item that has
 * the lowest key or the upper if they're the same.  We advance the
 * cursor past the item that is returned.
 *
 * XXX this will get fancier as we get range deletion items and
 * incremental update items.
 */
static int next_item(struct super_block *sb, struct compact_cursor *curs,
		     struct scoutfs_key_buf *item_key, struct kvec *item_val,
		     u8 *item_flags)
{
	struct compact_seg *upper = curs->upper;
	struct compact_seg *lower = curs->lower;
	struct scoutfs_key_buf lower_key;
	SCOUTFS_DECLARE_KVEC(lower_val);
	u8 lower_flags;
	int cmp;
	int ret;

retry:
	if (upper) {
		ret = scoutfs_seg_item_ptrs(upper->seg, upper->off,
					    item_key, item_val, item_flags);
		if (ret < 0)
			upper = NULL;
	}

	while (lower) {
		ret = read_segment(sb, lower);
		if (ret)
			goto out;

		ret = scoutfs_seg_item_ptrs(lower->seg, lower->off,
					    &lower_key, lower_val,
					    &lower_flags);
		if (ret == 0)
			break;
		lower = next_spos(curs, lower);
	}

	/* we're done if all are empty */
	if (!upper && !lower) {
		ret = 0;
		goto out;
	}

	/*
	 * < 0: return upper, advance upper
	 * == 0: return upper, advance both
	 * > 0: return lower, advance lower
	 */
	if (upper && lower)
		cmp = scoutfs_key_compare(item_key, &lower_key);
	else if (upper)
		cmp = -1;
	else
		cmp = 1;

	if (cmp > 0) {
		scoutfs_key_clone(item_key, &lower_key);
		scoutfs_kvec_clone(item_val, lower_val);
		*item_flags = lower_flags;
	}

	/*
	 * If we have a sticky compaction then we can't mix items from
	 * the upper level past the last lower key into the lower level.
	 * The caller will notice when they're emptying the final upper
	 * level in a sticky merge and leave it at the upper level.
	 */
	if (curs->sticky && curs->lower &&
	    (!lower || lower == curs->last_lower) &&
	    scoutfs_key_compare(item_key, curs->last_lower->last) > 0) {
		ret = 0;
		goto out;
	}

	if (cmp <= 0)
		upper->off = scoutfs_seg_next_off(upper->seg, upper->off);
	if (cmp >= 0)
		lower->off = scoutfs_seg_next_off(lower->seg, lower->off);

	/*
	 * Deletion items make their way down all the levels, replacing
	 * all the duplicate items that they find.  When we're
	 * compacting to the last level we can remove them by retrying
	 * the search after we've advanced past them.
	 */
	if ((curs->lower_level == curs->last_level) &&
	    ((*item_flags) & SCOUTFS_ITEM_FLAG_DELETION))
		goto retry;

	ret = 1;
out:
	curs->upper = upper;
	curs->lower = lower;

	return ret;
}

/*
 * Walk the input segments for items and append them to the output segment.
 * Items can exist in the input segments but not be written to the output
 * segment, for example if they're deletions.  The output segment can be
 * full.
 *
 * Return -errno if something went wrong, then 1 or 0 indicating items written.
 */
static int compact_items(struct super_block *sb, struct compact_cursor *curs,
			 struct scoutfs_segment *seg)
{
	struct scoutfs_key_buf item_key;
	SCOUTFS_DECLARE_KVEC(item_val);
	int has_next;
	int ret = 0;
	u8 flags;

	for (;;) {
		has_next = next_item(sb, curs, &item_key, item_val, &flags);
		if (has_next <= 0) {
			if (has_next < 0)
				ret = has_next;
			break;

		}

		if (scoutfs_seg_append_item(sb, seg, &item_key, item_val, flags,
					     curs->links))
			ret = 1;
		else
			break;
	}

	return ret;
}

static int compact_segments(struct super_block *sb,
			    struct compact_cursor *curs,
			    struct scoutfs_bio_completion *comp,
			    struct list_head *results)
{
	struct scoutfs_segment *seg;
	struct compact_seg *cseg;
	struct compact_seg *upper;
	struct compact_seg *lower;
	unsigned next_segno = 0;
	int ret = 0;

	scoutfs_inc_counter(sb, compact_operations);
	if (curs->sticky)
		scoutfs_inc_counter(sb, compact_sticky_upper);

	while (curs->upper || curs->lower) {

		upper = curs->upper;
		lower = curs->lower;

		/*
		 * If we're at the start of the upper segment and
		 * there's no lower segment then we might as well just
		 * move the segment in the manifest.  We can't do this
		 * if we're moving to the last level because we might
		 * need to drop any deletion items.
		 *
		 * XXX We should have metadata in the manifest to tell
		 * us that there's no deletion items in the segment.
		 */
		if (upper && upper->off == 0 && !lower && !curs->sticky &&
		    ((upper->level + 1) < curs->last_level)) {

			/*
			 * XXX blah!  these csegs are getting
			 * ridiculous.  We should have a robust manifest
			 * entry iterator that reading and compacting
			 * can use.
			 */
			cseg = alloc_cseg(sb, upper->first, upper->last);
			if (!cseg) {
				ret = -ENOMEM;
				break;
			}

			cseg->segno = upper->segno;
			cseg->seq = upper->seq;
			cseg->level = upper->level + 1;
			cseg->seg = upper->seg;
			if (cseg->seg)
				scoutfs_seg_get(cseg->seg);
			list_add_tail(&cseg->entry, results);

			/* don't mess with its segno */
			upper->part_of_move = true;
			cseg->part_of_move = true;

			curs->upper = NULL;

			scoutfs_inc_counter(sb, compact_segment_moved);
			break;
		}

		/* we're going to need its next key */
		ret = read_segment(sb, upper);
		if (ret)
			break;

		/*
		 * XXX we could intelligently skip reading and merging
		 * lower segments here.  The lower segment won't change
		 * if: 
		 *  - the lower segment is entirely before the upper
		 *  - the lower segment is full
		 *
		 * We don't have the metadata to determine that it's
		 * full today so we want to read lower segments that don't
		 * overlap so that we can merge partial lowers with
		 * its neighbours.
		 */

		ret = read_segment(sb, lower);
		if (ret)
			break;

		/* no cseg keys, manifest update uses seg item keys */
		cseg = kzalloc(sizeof(struct compact_seg), GFP_NOFS);
		if (!cseg) {
			ret = -ENOMEM;
			break;
		}

		cseg->segno = curs->segnos[next_segno];
		curs->segnos[next_segno] = 0;
		next_segno++;

		/*
		 * Compaction can free all the remaining items resulting
		 * in an empty output segment.  We just free it in that
		 * case.
		 */
		ret = scoutfs_seg_alloc(sb, cseg->segno, &seg);
		if (ret == 0)
			ret = compact_items(sb, curs, seg);
		if (ret < 1) {
			next_segno--;
			curs->segnos[next_segno] = cseg->segno;
			kfree(cseg);
			scoutfs_seg_put(seg);
			break;
		}

		/*
		 * The remaining upper items in a sticky merge have to
		 * be written into the upper level.
		 */
		if (curs->sticky && !lower) {
			cseg->level = curs->lower_level - 1;
			scoutfs_inc_counter(sb, compact_sticky_written);
		} else {
			cseg->level = curs->lower_level;
		}

		/* csegs will be claned up once they're on the list */
		cseg->seg = seg;
		list_add_tail(&cseg->entry, results);

		/* start a complete segment write now, we'll wait later */
		ret = scoutfs_seg_submit_write(sb, seg, comp);
		if (ret)
			break;

		scoutfs_inc_counter(sb, compact_segment_written);
	}

	return ret;
}

/*
 * Manifest walking is providing the details of the overall compaction
 * operation.
 */
void scoutfs_compact_describe(struct super_block *sb, void *data,
			      u8 upper_level, u8 last_level, bool sticky)
{
	struct compact_cursor *curs = data;

	curs->lower_level = upper_level + 1;
	curs->last_level = last_level;
	curs->sticky = sticky;
}

/*
 * Add a segment involved in the compaction operation.
 *
 * XXX Today we know that the caller is always adding only one upper segment
 * and is then possibly adding all the lower overlapping segments.
 */
int scoutfs_compact_add(struct super_block *sb, void *data,
			struct scoutfs_key_buf *first,
			struct scoutfs_key_buf *last, u64 segno, u64 seq,
			u8 level)
{
	struct compact_cursor *curs = data;
	struct compact_seg *cseg;
	int ret;

	cseg = alloc_cseg(sb, first, last);
	if (!cseg) {
		ret = -ENOMEM;
		goto out;
	}

	list_add_tail(&cseg->entry, &curs->csegs);

	cseg->segno = segno;
	cseg->seq = seq;
	cseg->level = level;

	if (!curs->upper)
		curs->upper = cseg;
	else if (!curs->lower)
		curs->lower = cseg;
	if (curs->lower)
		curs->last_lower = cseg;

	ret = 0;
out:
	return ret;
}

/*
 * Give the compaction cursor a segno to allocate from.
 */
void scoutfs_compact_add_segno(struct super_block *sb, void *data, u64 segno)
{
	struct compact_cursor *curs = data;

	curs->segnos[curs->nr_segnos++] = segno;
}

/*
 * Commit the result of a compaction based on the state of the cursor.
 * The net caller stops the rings from being written while we're making
 * changes.  We lock the manifest to atomically make our changes.
 *
 * The erorr handling is sketchy here because calling the manifest from
 * here is temporary.  We should be sending a message to the server
 * instead of calling the allocator and manifest.
 */
int scoutfs_compact_commit(struct super_block *sb, void *c, void *r)
{
	struct compact_cursor *curs = c;
	struct list_head *results = r;
	struct compact_seg *cseg;
	int ret;
	int i;

	/* free unused segnos that were allocated for the compaction */
	for (i = 0; i < curs->nr_segnos; i++) {
		if (curs->segnos[i]) {
			ret = scoutfs_alloc_free(sb, curs->segnos[i]);
			BUG_ON(ret);
		}
	}

	scoutfs_manifest_lock(sb);

	/* delete input segments, probably freeing their segnos */
	list_for_each_entry(cseg, &curs->csegs, entry) {
		if (!cseg->part_of_move) {
			ret = scoutfs_alloc_free(sb, cseg->segno);
			BUG_ON(ret);
		}

		ret = scoutfs_manifest_del(sb, cseg->first,
					   cseg->seq, cseg->level);
		BUG_ON(ret);
	}

	/* add output entries */
	list_for_each_entry(cseg, results, entry) {
		/* XXX moved upper segments won't have read the segment :P */
		if (cseg->seg)
			ret = scoutfs_seg_manifest_add(sb, cseg->seg,
						       cseg->level);
		else
			ret = scoutfs_manifest_add(sb, cseg->first,
						   cseg->last, cseg->segno,
						   cseg->seq, cseg->level);
		BUG_ON(ret);
	}

	scoutfs_manifest_unlock(sb);

	return 0;
}

/*
 * The compaction worker tries to make forward progress with compaction
 * every time its kicked.  It pretends to send a message requesting
 * compaction parameters but in reality the net request function there
 * is calling directly into the manifest and back into our compaction
 * add routines.
 *
 * We always try to clean up everything on errors.
 */
static void scoutfs_compact_func(struct work_struct *work)
{
	struct compact_info *ci = container_of(work, struct compact_info, work);
	struct super_block *sb = ci->sb;
	struct compact_cursor curs = {{NULL,}};
	struct scoutfs_bio_completion comp;
	struct compact_seg *cseg;
	LIST_HEAD(results);
	int ret;
	int err;

	INIT_LIST_HEAD(&curs.csegs);
	scoutfs_bio_init_comp(&comp);

	ret = scoutfs_net_get_compaction(sb, (void *)&curs);

	/* short circuit no compaction work to do */
	if (ret == 0 && list_empty(&curs.csegs))
		return;

	/* trace compaction ranges */
	list_for_each_entry(cseg, &curs.csegs, entry) {
		trace_scoutfs_compact_input(sb, cseg->level, cseg->segno,
					    cseg->seq, cseg->first, cseg->last);
	}

	if (ret == 0 && !list_empty(&curs.csegs)) {
		ret = compact_segments(sb, &curs, &comp, &results);

		/* always wait for io completion */
		err = scoutfs_bio_wait_comp(sb, &comp);
		if (!ret && err)
			ret = err;
	}

	/* don't update manifest on error, just free segnos */
	if (ret) {
		list_for_each_entry(cseg, &results, entry) {
			if (!cseg->part_of_move)
				curs.segnos[curs.nr_segnos++] = cseg->segno;
		}
		free_cseg_list(sb, &curs.csegs);
		free_cseg_list(sb, &results);
	}

	err = scoutfs_net_finish_compaction(sb, &curs, &results);
	if (!ret && err)
		ret = err;

	free_cseg_list(sb, &curs.csegs);
	free_cseg_list(sb, &results);

	WARN_ON_ONCE(ret);
	trace_printk("ret %d\n", ret);
}

void scoutfs_compact_kick(struct super_block *sb)
{
	DECLARE_COMPACT_INFO(sb, ci);

	queue_work(ci->workq, &ci->work);
}

int scoutfs_compact_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct compact_info *ci;

	ci = kzalloc(sizeof(struct compact_info), GFP_KERNEL);
	if (!ci)
		return -ENOMEM;

	ci->sb = sb;
	INIT_WORK(&ci->work, scoutfs_compact_func);

	ci->workq = alloc_workqueue("scoutfs_compact", 0, 1);
	if (!ci->workq) {
		kfree(ci);
		return -ENOMEM;
	}

	sbi->compact_info = ci;

	return 0;
}

/*
 * The system should be idle, there should not be any more manifest
 * modification which would kick compaction.
 */
void scoutfs_compact_destroy(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	DECLARE_COMPACT_INFO(sb, ci);

	if (ci) {
		flush_work(&ci->work);
		destroy_workqueue(ci->workq);
		sbi->compact_info = NULL;
	}
}
