#include "simple.h"

static inline struct simplefs_sb_info *SIMPLEFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct simplefs_inode *SIMPLEFS_INODE(struct inode *inode)
{
	return inode->i_private;
}
