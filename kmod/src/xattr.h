#ifndef _SCOUTFS_XATTR_H_
#define _SCOUTFS_XATTR_H_

extern const struct xattr_handler *scoutfs_xattr_handlers[];

ssize_t scoutfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
ssize_t scoutfs_list_xattrs(struct inode *inode, char *buffer,
			    size_t size, __u32 *hash_pos, __u64 *id_pos,
			    bool e_range, bool show_hidden);
int scoutfs_xattr_drop(struct super_block *sb, u64 ino,
		       struct scoutfs_lock *lock);

struct scoutfs_xattr_prefix_tags {
	unsigned long hide:1,
		      srch:1,
		      totl:1;
};

int scoutfs_xattr_parse_tags(const char *name, unsigned int name_len,
			     struct scoutfs_xattr_prefix_tags *tgs);

void scoutfs_xattr_init_totl_key(struct scoutfs_key *key, u64 *name);
int scoutfs_xattr_combine_totl(void *dst, int dst_len, void *src, int src_len);

#endif
