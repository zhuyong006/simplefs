/*
 * A Simple Filesystem for the Linux Kernel.
 *
 * Initial author: Sankar P <sankar.curiosity@gmail.com>
 * License: Creative Commons Zero License - http://creativecommons.org/publicdomain/zero/1.0/
 *
 * TODO: we need to split it into smaller files
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/time64.h>

#include "super.h"

#define f_dentry f_path.dentry
/* A super block lock that must be used for any critical section operation on the sb,
 * such as: updating the free_blocks, inodes_count etc. */
static DEFINE_MUTEX(simplefs_sb_lock);
static DEFINE_MUTEX(simplefs_inodes_mgmt_lock);
#if 1
#define CDBG(fmt, args...) printk(fmt, ##args)
#else
#define CDBG(fmt, args...)
#endif
/* FIXME: This can be moved to an in-memory structure of the simplefs_inode.
 * Because of the global nature of this lock, we cannot create
 * new children (without locking) in two different dirs at a time.
 * They will get sequentially created. If we move the lock
 * to a directory-specific way (by moving it inside inode), the
 * insertion of two children in two different directories can be
 * done in parallel */
static DEFINE_MUTEX(simplefs_directory_children_update_lock);

static struct kmem_cache *sfs_inode_cachep;
static struct kmem_cache *sfs_entry_cachep;

/*代表目录中每一项的信息*/
struct simplefs_cache_entry {
	struct simplefs_dir_record record;
	struct list_head list;
	int entry_no;
};

/*代表整个目录*/
struct simplefs_dir_cache {
	uint64_t dir_children_count;
	struct list_head used;
	struct list_head free;
};



static struct simplefs_dir_cache *simplefs_cache_alloc(void)
{
    struct simplefs_dir_cache *dir_cache;
    dir_cache = kzalloc(sizeof(struct simplefs_dir_cache), GFP_KERNEL);
    if (!dir_cache)
        return ERR_PTR(-ENOMEM);

    INIT_LIST_HEAD(&dir_cache->free);
    INIT_LIST_HEAD(&dir_cache->used);

    return dir_cache;
}

/*         参数说明
    dir_cache:
    			  当前目录的缓存
    bh:
    			  磁盘中当前目录存放的实际信息
 */


static int dir_cache_build(struct simplefs_dir_cache *dir_cache, struct buffer_head *bh)
{
	struct simplefs_dir_record *record;
	struct simplefs_cache_entry *cache_entry;
	int i;

	record = (struct simplefs_dir_record *)bh->b_data;

	//SIMPLEFS_MAX_CHILDREN_CNT代表该目录最多可以支持创建的Inode总数
	//因为目前项这个DATA_BLOCK中存放的都是simplefs_dir_record，因此最多存放数
	//就是数据块大小/单个Entry的值
	for (i = 0; i < SIMPLEFS_DEFAULT_BLOCK_SIZE/sizeof(struct simplefs_dir_record); i++, record++) {
		//为目录中的每一项内容分配一个缓存
		cache_entry = kmem_cache_alloc(sfs_entry_cachep, GFP_KERNEL);
		if (!cache_entry)
			return -ENOMEM;
		//内容序号进行设置
		cache_entry->entry_no = i;
		
		//Inode的编号是从1开始的，因此如果为0，说明该目录下的这个Inode已经释放
		if (record->inode_no != 0) {
			//如果不为0，插入到目录的缓存中的used链表中
			memcpy(&cache_entry->record, record, sizeof(struct simplefs_dir_record));
			list_add_tail(&cache_entry->list, &dir_cache->used);
		} else {
			//如果为0，则说明该Inode已经被释放，则插入到目录缓存的free链表中
			list_add_tail(&cache_entry->list, &dir_cache->free);
		}
	}

	return 0;
}

/*         参数说明
    dir_cache:
    			  当前目录的缓存
    dentry:
    			  需要查找的entry项
 */
static struct simplefs_cache_entry *used_cache_entry_get(struct simplefs_dir_cache *dir_cache,struct dentry *dentry)
{
	struct simplefs_cache_entry *cache_entry;

	list_for_each_entry(cache_entry, &dir_cache->used, list) {
		if (!strcmp(cache_entry->record.filename, dentry->d_name.name)) {
			return cache_entry;
		}
	}

	return NULL;
}
/*         参数说明
    dir_cache:
    			  当前目录的缓存
    返回值:
    			  从当前目录缓存中的free链表中,返回第一个空闲的元素
    			  
 */
static struct simplefs_cache_entry *free_cache_entry_get(struct simplefs_dir_cache *dir_cache)
{
	return list_first_entry(&dir_cache->free, struct simplefs_cache_entry, list);
}

/*         参数说明
    head:
    			  目录缓存的链表头(used,free都有可能)
    cache_entry:  即将插入used/free链表头的entry项
    			  
    			  
 */

static void cache_entry_insert(struct list_head *head, struct simplefs_cache_entry *cache_entry)
{
	struct simplefs_cache_entry *tmp_entry;
	list_del(&cache_entry->list);

	list_for_each_entry(tmp_entry, head, list) {
		if (cache_entry->entry_no < tmp_entry->entry_no)
			break;
	}

	list_add_tail(&cache_entry->list, &tmp_entry->list);
}


//同步超级块
void simplefs_sb_sync(struct super_block *vsb)
{
	struct buffer_head *bh;
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb)->sb;
	
	bh = sb_bread(vsb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);

	bh->b_data = (char *)sb;
	/* 标记缓冲区首部为脏 */
	mark_buffer_dirty(bh);
	/* 然后同步 */
	sync_dirty_buffer(bh);
	/*释放bh指针*/
	brelse(bh);
}

struct simplefs_inode *simplefs_inode_search(struct super_block *sb,
		struct simplefs_inode *start,
		struct simplefs_inode *search)
{
	uint64_t count = 0;
	//每个数据块是4K，这个数据块全部存放的是Inode，因此Inode的总数如下：
	int icount = SIMPLEFS_DEFAULT_BLOCK_SIZE / sizeof(struct simplefs_inode);
	while (start->inode_no != search->inode_no && count < icount) {
		count++;
		start++;
	}

	if (start->inode_no == search->inode_no) {
		return start;
	}

	return NULL;
}
		
/*         参数说明
    vsb:
    			  超级块
    inode:
    			  即将插入到Inode Block Data的inode
    			  
 */

void simplefs_inode_add(struct super_block *vsb, struct simplefs_inode *inode)
{
	struct simplefs_sb_info *sb_info = SIMPLEFS_SB(vsb);
	struct buffer_head *bh;
	struct simplefs_inode *inode_iterator;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	//存放Inode信息的数据区
	bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);
	//因为这个数据区依次存放的都是simplefs_inode的结构，因此做下强制转换
	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	/* Append the new inode in the end in the inode store */
	/*先将inode_iterator指向inode_no对应的存储区*/
	inode_iterator += inode->inode_no-1;
	//拷贝Inode信息到对应的位置
	memcpy(inode_iterator, inode, sizeof(struct simplefs_inode));
	//将超级块中的Inode总数计数自增
	sb_info->sb->inodes_count++;
	//将超级块中的Inode bitmap的对应位置位
	set_bit(inode->inode_no, &sb_info->imap);

	//先将当前的数据块标记为脏，等待回写磁盘
	mark_buffer_dirty(bh);
	//同理超级块也需要更新
	simplefs_sb_sync(vsb);
	/*释放Inode的数据块*/
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);
}

/*         参数说明
    vsb:
    			  超级块
    inode:
    			  即将删除的Inode
    			  
 */

void simplefs_inode_del(struct super_block *vsb, struct simplefs_inode *inode)
{
	struct simplefs_sb_info *sb_info = SIMPLEFS_SB(vsb);
	struct buffer_head *bh;
	struct simplefs_inode *inode_iterator;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	//存放Inode信息的数据区
	bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);
	//因为这个数据区依次存放的都是simplefs_inode的结构，因此做下强制转换
	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	/* Append the new inode in the end in the inode store */
	/*先将inode_iterator指向inode_no对应的存储区*/
	inode_iterator += inode->inode_no-1;
	//拷贝Inode信息到对应的位置
	memset(inode_iterator, 0x0, sizeof(struct simplefs_inode));
	//将超级块中的Inode总数计数自减
	sb_info->sb->inodes_count--;
	//将超级块中的Inode bitmap的对应位置位
	clear_bit(inode->inode_no, &sb_info->imap);

	//先将当前的数据块标记为脏，等待回写磁盘
	mark_buffer_dirty(bh);
	//同理超级块也需要更新
	simplefs_sb_sync(vsb);
	/*释放Inode的数据块*/
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);
}

/* This function returns a blocknumber which is free.
 * The block will be removed from the freeblock list.
 *
 * In an ideal, production-ready filesystem, we will not be dealing with blocks,
 * and instead we will be using extents
 *
 * If for some reason, the file creation/deletion failed, the block number
 * will still be marked as non-free. You need fsck to fix this.*/
// sb->free_blocks对应的Bit位如果为1，那么说明对应的数据块空闲，否则该数据块Busy

/*         参数说明
    vsb:
    			  超级块
    out:
    			  即将返回的空闲数据块的索引
    			  
 */
int simplefs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out)
{
	//通过内核标准的SuperBlock结构获取特定文件系统的SB结构
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb)->sb;
	int i;
	int ret = 0;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		ret = -EINTR;
		goto end;
	}

	//需要注意的是，数据块是从第3个开始的，原因是超级块，inode信息区，根节点内容区，本身已经在文件系统了
	// sb->free_blocks中的每一个Bit代表了一个数据块
	/* Loop until we find a free block. We start the loop from 3,
	 * as all prior blocks will always be in use */
	for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
		//如果sb->free_blocks的对应Bit为0，则意味着当前数据块是空闲的可以使用
		if (sb->free_blocks & (1 << i)) {
			break;
		}
	}

	//如果上面的循环从3~SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED,找了一圈还是没有找到空闲的数据块
	//则说明该文件系统没有剩余的空间了，返回出错信息
	if (unlikely(i == SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "No more free blocks available");
		ret = -ENOSPC;
		goto end;
	}

	//如果找到空闲的数据块，则返回该数据块的索引
	*out = i;

	//既然找到了空闲的数据块，那么需要将sb->free_blocks的对应Bit置位
	/* Remove the identified block from the free list */
	sb->free_blocks &= ~(1 << i);

	//另外我们还需要将对应的数据块标记为dirty，并回写到磁盘
	simplefs_sb_sync(vsb);

end:
	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

/*返回当前文件系统中的Inode总数*/
static int simplefs_sb_get_objects_count(struct super_block *vsb,
					 uint64_t * out)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb)->sb;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	*out = sb->inodes_count;
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
/*这个函数在"ls"指令的时候会被调度到*/
/*         参数说明
    filp:
    			  当前目录的文件指针
    ctx:
    			  显示上下文信息

    说明：从当前目录的缓存中取出已使用的entry链表，逐个上报给vfs
 */
static int simplefs_iterate(struct file *filp, struct dir_context *ctx)
#else
static int simplefs_readdir(struct file *filp, void *dirent, filldir_t filldir)
#endif
{
	loff_t pos;
	//通过目录的文件指针，获取目录结构
	struct dentry *dentry = filp->f_path.dentry;
	//获取当前目录的Inode
	struct inode * parent_inode = dentry->d_inode;
	//目录缓存指针
	struct simplefs_dir_cache *dir_cache = NULL;
	//临时变量，记录缓存中的每一个文件
	struct simplefs_cache_entry *cache_entry;
	//通过内核的inode指针获取特定文件系统的inode缓存指针
	struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
	struct buffer_head *bh;

	CDBG("dentry inode no: %d\n",parent_inode->i_ino);
	
	dir_cache = (struct simplefs_dir_cache *)dentry->d_fsdata;

	//如果目录的缓存不存在，则为这个目录创建一个新的缓存
	if (!dir_cache) {
		CDBG("new simplefs_dir_cache\n");
		//为当前的目录分配一个缓存
		dir_cache = simplefs_cache_alloc();
		if (IS_ERR(dentry->d_fsdata))
		    return -EINVAL;
		//从当前目录中获取信息
		bh = sb_bread(parent_inode->i_sb, parent->data_block_number);
		BUG_ON(!bh);
		//为当前目录项的已用和空闲项创建链表索引
		dir_cache_build(dir_cache, bh);
		brelse(bh);
	}
	dentry->d_fsdata = dir_cache;

	pos = ctx->pos;
	
	if (pos) {
		/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
		 * We should probably fix this to work in a cursor based model and
		 * use the tokens correctly to not fill too many data in each cursor based call */
		return 0;
	}

	list_for_each_entry(cache_entry, &dir_cache->used, list) {
		dir_emit(ctx, cache_entry->record.filename, SIMPLEFS_FILENAME_MAXLEN,
			cache_entry->record.inode_no, DT_UNKNOWN);		
		ctx->pos += sizeof(struct simplefs_dir_record);
		pos += sizeof(struct simplefs_dir_record);
	}


	return 0;
}

/* This functions returns a simplefs_inode with the given inode_no
 * from the inode store, if it exists. */
//从Inode的起始域开始根据Inode号查询到对应的Inode信息，并返回。
struct simplefs_inode *simplefs_get_inode(struct super_block *sb,
					  uint64_t inode_no)
{
	struct simplefs_inode *sfs_inode = NULL;
	struct simplefs_inode *inode_buffer = NULL;

	struct buffer_head *bh;

	/* The inode store can be read once and kept in memory permanently while mounting.
	 * But such a model will not be scalable in a filesystem with
	 * millions or billions of files (inodes) */
	//指向Inode的起始域
	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);
	//将其强制转换为simplefs_inode类型的指针
	sfs_inode = (struct simplefs_inode *)bh->b_data;
	
	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return NULL;
	}

	if(inode_no == 0)
	{
		printk("%s invalid inode_no\n",__func__);
	}
	
	//分配一个空闲的Inode缓存，并将磁盘中的Inode信息拷贝出来
	inode_buffer = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
	//磁盘中Inode的信息区是按照顺序存放的
	/*  
	 *  根节点 占用了1号Inode，其余的Inode依次排放
	 */
	sfs_inode += inode_no-1;
	memcpy(inode_buffer, sfs_inode, sizeof(struct simplefs_inode));
	
	mutex_unlock(&simplefs_inodes_mgmt_lock);
	brelse(bh);
	return inode_buffer;
}

ssize_t simplefs_read(struct file * filp, char __user * buf, size_t len,
		      loff_t * ppos)
{
	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct simplefs_inode *inode =
	    SIMPLEFS_INODE(filp->f_path.dentry->d_inode);
	struct buffer_head *bh;

	char *buffer;
	int nbytes;

	//如果要读数据的偏移超出了该Inode的大小，那么直接返回读取长度为0
	if (*ppos >= inode->file_size) {
		/* Read request with offset beyond the filesize */
		return 0;
	}

	//得到该Inode的数据区，将其读取出来
	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       inode->data_block_number);
		return 0;
	}
	//将数据区强制转换为Char*
	buffer = (char *)bh->b_data;
	//既然是读，就要考虑到有可能你读取的长度会超过该Inode的大小，因此需要取俩者中的最大值
	nbytes = min((size_t) inode->file_size, len);
	//该Inode读取到的数据传递给用户层
	if (copy_to_user(buf, buffer, nbytes)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents to the userspace buffer\n");
		return -EFAULT;
	}
	//由于读取操作不会改变磁盘的内容因此不需要同步操作，直接释放数据块的指针
	brelse(bh);
	//改变游标的指针
	*ppos += nbytes;
	//返回读取的长度
	return nbytes;
}

/* Save the modified inode */
int simplefs_inode_save(struct super_block *sb, struct simplefs_inode *sfs_inode)
{
	struct simplefs_inode *inode_iterator;
	struct buffer_head *bh;
	//先读取Inode的数据区
	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//从Inode的数据区中匹配要更新的Inode
	inode_iterator = simplefs_inode_search(sb,
		(struct simplefs_inode *)bh->b_data,
		sfs_inode);

	if (likely(inode_iterator)) {
		/*更新Inode*/
		memcpy(inode_iterator, sfs_inode, sizeof(*inode_iterator));
		CDBG(KERN_INFO "The inode updated\n");
		//将Inode的数据区设置为Dirty，并同步
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	} else {
		mutex_unlock(&simplefs_sb_lock);
		printk(KERN_ERR
		       "The new filesize could not be stored to the inode.");
		return -EIO;
	}
	//释放该数据区
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);

	return 0;
}

/* FIXME: The write support is rudimentary. I have not figured out a way to do writes
 * from particular offsets (even though I have written some untested code for this below) efficiently. */
ssize_t simplefs_write(struct file * filp, const char __user * buf, size_t len,
		       loff_t * ppos)
{
	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct buffer_head *bh;
	struct super_block *sb;

	char *buffer;

	int retval;

#if 0
	retval = generic_write_checks(filp, ppos, &len, 0);
	if (retval) {
		return retval;
	}
#endif
	//通过文件得到内核对应的Inode指针
	inode = filp->f_path.dentry->d_inode;
	//通过内核的Inode指针进而得到特定文件系统的Inode指针
	sfs_inode = SIMPLEFS_INODE(inode);
	//通过Inode得到SuperBlock
	sb = inode->i_sb;
	//获取该Inode指向的数据块
	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    sfs_inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       sfs_inode->data_block_number);
		return 0;
	}
	
	//将数据区强制转换为char*型
	buffer = (char *)bh->b_data;

	/* Move the pointer until the required byte offset */
	//移动到vfs指定的偏移位置
	buffer += *ppos;

	//拷贝用户空间的数据到对应的数据块中
	if (copy_from_user(buffer, buf, len)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents from the userspace buffer to the kernel space\n");
		return -EFAULT;
	}
	//通知VFS指针偏移了多少
	*ppos += len;
	//将数据区设置为Dirty，并回写到磁盘
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	//最后释放数据块的指针
	brelse(bh);

	/* Set new size
	 * sfs_inode->file_size = max(sfs_inode->file_size, *ppos);
	 *
	 * FIXME: What to do if someone writes only some parts in between ?
	 * The above code will also fail in case a file is overwritten with
	 * a shorter buffer */
	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//更新Inode的文件大小
	sfs_inode->file_size = *ppos;
	//既然更新了Inode的信息，那么Inode的信息区也要同步更新下
	retval = simplefs_inode_save(sb, sfs_inode);
	if (retval) {
		len = retval;
	}
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return len;
}

const struct file_operations simplefs_file_operations = {
	.read = simplefs_read,
	.write = simplefs_write,
};

const struct file_operations simplefs_dir_operations = {
	.owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	.iterate = simplefs_iterate,
#else
	.readdir = simplefs_readdir,
#endif
};

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags);

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode);
static int simplefs_unlink(struct inode *dir,struct dentry *dentry);

static struct inode_operations simplefs_inode_ops = {
	.create = simplefs_create,
	.lookup = simplefs_lookup,
	.mkdir = simplefs_mkdir,
	.unlink = simplefs_unlink,
	
};
/*
 *        		函数参数说明
 *    dir：    			当前所在目录的Inode
 *    dentry:  			dentry->d_name.name:待创建的文件或者目录项
 */
static int simplefs_create_fs_object(struct inode *dir, struct dentry *dentry,
				     umode_t mode)
{
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	uint64_t count;
	int ret;
	struct super_block *sb = dir->i_sb;
	struct dentry *parent_dentry = dentry->d_parent;
	struct simplefs_cache_entry *cache_entry;
	struct simplefs_dir_cache * dir_cache;
	struct simplefs_sb_info *sb_info = SIMPLEFS_SB(sb);


	BUG_ON(parent_dentry->d_inode != dir);

	dir_cache = (struct simplefs_dir_cache *)parent_dentry->d_fsdata;

	BUG_ON(!dir_cache);
	
	if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//通过这个父Inode获取到这个文件系统的SuperBlock
	sb = dir->i_sb;
	
	//我们首先思考，我们如果想要创建一个Inode是不是应该看下该文件系统的Inode位置是否		
	//还有空余来允许我们创建呢，因此，我们先要得到当前文件系统已经使用的Inode总数。
	ret = simplefs_sb_get_objects_count(sb, &count);
	if (ret < 0) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}

	//先判断Inode总数是否超了，如果是，则返回用户没有空间创建了
	if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		/* The above condition can be just == insted of the >= */
		printk(KERN_ERR
		       "Maximum number of objects supported by simplefs is already reached");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOSPC;
	}

	//该文件系统只支持目录和普通文件的创建，否则返回出错
	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		printk(KERN_ERR
		       "Creation request but for neither a file nor a directory");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -EINVAL;
	}
	
	//通过SuperBlock创建一个空的Inode  
	inode = new_inode(sb);
	if (!inode) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOMEM;
	}
	CDBG("sb imap inode no = %d\n",ffz(sb_info->imap));
	//设置这个Inode指向的SuperBlock   
	inode->i_sb = sb;
	//设置这个Inode的操作指针
	inode->i_op = &simplefs_inode_ops;
	//设置这个Inode的创建时间
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	//从超级块的inode map中取出第一个为0的索引号(Bit位)
	inode->i_ino = ffz(sb_info->imap);
	//创建特定文件系统的Inode结构
	sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
	//对该节点的Inode号赋值
	sfs_inode->inode_no = inode->i_ino;
	//将内核标准节点的私有指针指向当前特定文件系统的Inode结构
	inode->i_private = sfs_inode;
	//设置文件系统的属性
	sfs_inode->mode = mode;

	//对文件目录以及普通文件分别做设置，需要注意的是，如果创建的是一个目录，那么毫无疑问，当前目录下
	//的Inode个数肯定还是为0的
	if (S_ISDIR(mode)) {
		CDBG(KERN_INFO "New directory creation request\n");
		sfs_inode->dir_children_count = 0;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(mode)) {
		CDBG(KERN_INFO "New file creation request\n");
		sfs_inode->file_size = 0;
		//针对普通文件设置读写操作
		inode->i_fop = &simplefs_file_operations;
	}

	/* First get a free block and update the free map,
	 * Then add inode to the inode store and update the sb inodes_count,
	 * Then update the parent directory's inode with the new child.
	 *
	 * The above ordering helps us to maintain fs consistency
	 * even in most crashes
	 */
	//从超级块中获取空闲的数据块
	ret = simplefs_sb_get_a_freeblock(sb, &sfs_inode->data_block_number);
	if (ret < 0) {
		printk(KERN_ERR "simplefs could not get a freeblock");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}
	//新建一个Inode需要更新Inode的数据区，并同步
	simplefs_inode_add(sb, sfs_inode);

	/*从当前目录缓存中的free链表中取出一个空闲的entry项*/
	cache_entry = free_cache_entry_get(dir_cache);

	/*除了更新Inode的数据区，我们还需要做一件事:在父目录(Inode)下面，添加该Inode的信息*/
	//既然要添加信息，必须首先取得当前父目录的结构信息，通过内核的标准Inode获取特定文件系统的Inode
	//信息
	parent_dir_inode = SIMPLEFS_INODE(dir);
	//通过simplefs_inode中的成员从而获取到数据信息
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	BUG_ON(!bh);
	//需要知道的是目录Inode中存放的内容结构都是固定的，因此做下强制转换
	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;

	/* Navigate to the last record in the directory contents */
	/*需要注意的是父目录缓存中的entry也是按照顺序存放的，cache_entry->entry_no指向的
      就是存放的序号，我们从free链表中取出的entry是紧跟在used后面的
	*/
	dir_contents_datablock += cache_entry->entry_no;
	/*磁盘中的目录entry信息更新*/
	dir_contents_datablock->inode_no = sfs_inode->inode_no;
	strcpy(dir_contents_datablock->filename, dentry->d_name.name);
	/*拷贝磁盘中目录单项存放的内容到entry的缓存中*/
	memcpy(&cache_entry->record, dir_contents_datablock,sizeof(struct simplefs_dir_record));
	/*将当前的cache_entry插入到used的链表中*/
	cache_entry_insert(&dir_cache->used, cache_entry);

	/*将父目录指向的数据块设置为dirty，并将其回写到磁盘，之后释放*/
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//将父目录中的dir_children_count也自增
	parent_dir_inode->dir_children_count++;
	//同理我们更改了Inode数据区，这个数据区自然也要同步下了
	ret = simplefs_inode_save(sb, parent_dir_inode);
	if (ret) {
		mutex_unlock(&simplefs_inodes_mgmt_lock);
		mutex_unlock(&simplefs_directory_children_update_lock);

		/* TODO: Remove the newly created inode from the disk and in-memory inode store
		 * and also update the superblock, freemaps etc. to reflect the same.
		 * Basically, Undo all actions done during this create call */
		return ret;
	}

	mutex_unlock(&simplefs_inodes_mgmt_lock);
	mutex_unlock(&simplefs_directory_children_update_lock);
	//将当前Inode和其父目录关联
	inode_init_owner(inode, dir, mode);
	//将当前inode绑定到dentry中
	d_add(dentry, inode);

	return 0;
}

/*
 *        		函数参数说明
 *    dir：    			当前所在目录的Inode
 *    dentry:  			dentry->d_name.name:待删除的文件
 */
static int simplefs_unlink(struct inode *dir,struct dentry *dentry)
{
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	uint64_t count;
	int ret;
	struct super_block *sb = dir->i_sb;
	struct dentry *parent_dentry = dentry->d_parent;
	struct simplefs_cache_entry *cache_entry;
	struct simplefs_dir_cache * dir_cache;
	struct simplefs_sb_info *sb_info = SIMPLEFS_SB(sb);

	/*获取待删除文件对应该文件系统的inode缓存*/
	sfs_inode = SIMPLEFS_INODE(dentry->d_inode);

	dir_cache = (struct simplefs_dir_cache *)parent_dentry->d_fsdata;

	/*将目录中对应的项清除*/
	parent_dir_inode = SIMPLEFS_INODE(dir);
	//通过simplefs_inode中的成员从而获取到数据信息
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;
	cache_entry = used_cache_entry_get(dir_cache,dentry);
	dir_contents_datablock += cache_entry->entry_no;
	memset(dir_contents_datablock,0x0,sizeof(struct simplefs_dir_record));
	/*将父目录指向的数据块设置为dirty，并将其回写到磁盘，之后释放*/
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	/*从当前目录的used链表中，删除当前的cache_entry*/
	list_del(&cache_entry->list);
	//将释放的cache_entry插入到当前目录的free链表中
	list_add(&cache_entry->list,&dir_cache->free);
	//将目录下的inode数减一
	dir_cache->dir_children_count--;
	//将父目录中的dir_children_count也自减
	parent_dir_inode->dir_children_count--;
	//同理我们更改了Inode数据区，这个数据区自然也要同步下了
	ret = simplefs_inode_save(sb, parent_dir_inode);

	//在Inode的存储区中，清除对应的Inode
	simplefs_inode_del(sb,sfs_inode);

	//释放内核的inode结构
	__destroy_inode(dentry->d_inode);

	dput(dentry);

	return 0;
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode)
{
	/* I believe this is a bug in the kernel, for some reason, the mkdir callback
	 * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
	 
	CDBG("%s LINE = %d\n",__func__,__LINE__);
	return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl)
{
	CDBG("%s LINE = %d\n",__func__,__LINE__);

	return simplefs_create_fs_object(dir, dentry, mode);
}
/*         参数说明
    parent_inode:
    			  当前目录的Inode
    child_dentry:
    			   当前查询的Inode,child_dentry->d_name.name可以获取名称
    flags:
    
 */
struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags)
{
	struct super_block *sb = parent_inode->i_sb;
	struct dentry *parent_dentry;
	struct simplefs_dir_cache *dir_cache;
	struct simplefs_cache_entry *cache_entry;
	struct inode *inode;
	struct simplefs_inode *sfs_inode;

	CDBG("%s LINE = %d,%s\n",__func__,__LINE__,
		child_dentry->d_name.name);
	
	//得到父目录
	parent_dentry = child_dentry->d_parent;
	
	if (parent_dentry->d_inode != parent_inode)
		return ERR_PTR(-ENOENT);
	
	//得到父目录的私有数据: 目录Cache
	dir_cache = (struct simplefs_dir_cache *)parent_dentry->d_fsdata;
	
	CDBG("%s LINE = %d\n",__func__,__LINE__);

	//从目录cache中的used链表中找到，当前查询文件的cache_entry
	cache_entry = used_cache_entry_get(dir_cache, child_dentry);

	//如果cache_entry为空，说明该文件不在目录中，需要creat
	if (!cache_entry)
		goto out;
	
	CDBG("%s check inode_no = %d\n",__func__,cache_entry->record.inode_no);

	
	sfs_inode = simplefs_get_inode(sb, cache_entry->record.inode_no);
	if (!sfs_inode)
		return ERR_PTR(-ENOENT);

	
	inode = new_inode(sb);
	inode->i_ino = cache_entry->record.inode_no;
	inode_init_owner(inode, parent_inode, sfs_inode->mode);
	inode->i_sb = sb;
	inode->i_op = &simplefs_inode_ops;

	if (S_ISDIR(inode->i_mode))
		inode->i_fop = &simplefs_dir_operations;
	else if (S_ISREG(inode->i_mode))
		inode->i_fop = &simplefs_file_operations;
	else
		printk(KERN_ERR
		       "Unknown inode type. Neither a directory nor a file");

	/* FIXME: We should store these times to disk and retrieve them */
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;

	inode->i_private = sfs_inode;

	d_add(child_dentry, inode);
	
	CDBG(KERN_ERR
	       "No inode found for the filename [%s]\n",
	       child_dentry->d_name.name);

out:
	return NULL;

}


/**
 * Simplest
 */
void simplefs_destory_inode(struct inode *inode)
{
	struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);

	printk(KERN_INFO "Freeing private data of inode %p (%lu)\n",
	       sfs_inode, inode->i_ino);
	kmem_cache_free(sfs_inode_cachep, sfs_inode);
}

static const struct super_operations simplefs_sops = {
	.destroy_inode = simplefs_destory_inode,
};

static void simplefs_dentry_release(struct dentry *dentry)
{
	struct simplefs_dir_cache *dir_cache = dentry->d_fsdata;
	struct simplefs_cache_entry *tmp, *cache_entry;

	if (dir_cache) {
		list_for_each_entry_safe(cache_entry, tmp, &dir_cache->free, list) {
		list_del(&cache_entry->list);
		kmem_cache_free(sfs_entry_cachep, cache_entry);
		}

		list_for_each_entry_safe(cache_entry, tmp, &dir_cache->used, list) {
		list_del(&cache_entry->list);
		kmem_cache_free(sfs_entry_cachep, cache_entry);
		}
	}

	kfree(dir_cache);
	dentry->d_fsdata = NULL;
}

static const struct dentry_operations simplefs_dentry_operations = {
	.d_release = simplefs_dentry_release,
};

static void fill_imap(struct super_block *sb)
{
	int i;
	struct simplefs_sb_info *sb_info = sb->s_fs_info;
	struct simplefs_inode *simple_inode;
	struct buffer_head *bh;
	int icount = SIMPLEFS_DEFAULT_BLOCK_SIZE / sizeof(struct simplefs_inode);

	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	simple_inode = (struct simplefs_inode *)bh->b_data;
	
	CDBG("%s start,root inode = %d\n",__func__,simple_inode->inode_no);
	CDBG("sb imap inode no = %d\n",ffz(sb_info->imap));

	/*第1个bit预留不用，因为root节点的序号是从1开始的*/
	for (i = 0; i < SIMPLEFS_START_INO; i++)
		set_bit(i, &sb_info->imap);

	/*从inode的元数据区中，逐个比对，将已经使用的inode在bitmap中标记*/
	for (i = SIMPLEFS_START_INO; i < icount; i++) {
		if (simple_inode->inode_no != 0) {
			printk("func %s, line %d, ino %lld\n", __func__, __LINE__, simple_inode->inode_no);
			set_bit(simple_inode->inode_no, &sb_info->imap);
		}
		simple_inode++;
	}
	
	CDBG("sb imap inode no = %d\n",ffz(sb_info->imap));
	CDBG("%s end\n",__func__);

	brelse(bh);
}


/* This function, as the name implies, Makes the super_block valid and
 * fills filesystem specific information in the super block */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct simplefs_super_block *sb_disk;
	struct simplefs_sb_info *sb_info;
	int ret = -EPERM;

	sb_info = kzalloc(sizeof(struct simplefs_sb_info),GFP_KERNEL);
	bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);
	//获取磁盘中存放的super block的真实内容
	sb_disk = (struct simplefs_super_block *)bh->b_data;

	//设置超级块缓存指向的内核sb的指针
	sb_info->sb = sb_disk;
	//设置超级块缓存指向的buffer_head
	sb_info->bh = bh;

	printk(KERN_INFO "The magic number obtained in disk is: [%llu]\n",
	       sb_disk->magic);

	if (unlikely(sb_disk->magic != SIMPLEFS_MAGIC)) {
		printk(KERN_ERR
		       "The filesystem that you try to mount is not of type simplefs. Magicnumber mismatch.");
		goto release;
	}

	if (unlikely(sb_disk->block_size != SIMPLEFS_DEFAULT_BLOCK_SIZE)) {
		printk(KERN_ERR
		       "simplefs seem to be formatted using a non-standard block size.");
		goto release;
	}

	printk(KERN_INFO
	       "simplefs filesystem of version [%llu] formatted with a block size of [%llu] detected in the device.\n",
	       sb_disk->version, sb_disk->block_size);

	/* A magic number that uniquely identifies our filesystem type */
	//sb中的魔数和磁盘中的一致
	sb->s_magic = SIMPLEFS_MAGIC;

	/* For all practical purposes, we will be using this s_fs_info as the super block */
	//使得内核的sb私有指针指向超级块的缓存
	sb->s_fs_info = sb_info;
	//表明当前文件系统最大数据块为4K
	sb->s_maxbytes = SIMPLEFS_DEFAULT_BLOCK_SIZE;
	//实现Inode的destroy指针，当文件系统的文件被删除后，其对应的Inode缓存会被其
	//函数指针的函数释放
	sb->s_op = &simplefs_sops;
	
	sb->s_d_op = &simplefs_dentry_operations;
	/*更新超级块缓存中存放的inode bitmap*/
	fill_imap(sb);

	//为我们的根节点分配一个Inode
	root_inode = new_inode(sb);
	//设置根节点的Inode编号
	root_inode->i_ino = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	//声明这个Inode是一个目录，因为是根dentry所以它的所在目录为NULL
	inode_init_owner(root_inode, NULL, S_IFDIR);
	//指向所在文件系统的超级块
	root_inode->i_sb = sb;
	//因为需要在根节点下进行操作，因此需要实现节点的操作指针
	/*
	 * 	1.创建一个普通文件，调用create指针;
	 * 	2.创建一个普通文件之前，还需要先调用lookup指针;
	 * 	3.创建一个目录，调用mkdir;
	 */
	root_inode->i_op = &simplefs_inode_ops;
	//Provide Io Operation For UserSpace,eg:readdir
	//是否需要支持文件的操作，比如读写,mmap等
	root_inode->i_fop = &simplefs_dir_operations;
	//Inode的创建时间戳
	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime =
	    CURRENT_TIME;
	//既然是根节点，自然是需要获取根节点的内容，这里的i_private就是指向根节点信息的
	/*
		信息如下:
		1.根节点号
		2.根节点中数据内容存放的位置-> DATA_BLOCK_BUMBER
		3.根节点下面的子节点个数
	*/
	root_inode->i_private =
	    simplefs_get_inode(sb, SIMPLEFS_ROOTDIR_INODE_NUMBER);

	
	/* TODO: move such stuff into separate header. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
	//Super Block需要告知当前文件系统的根dentry，而这个dentry本质上也是来自于一个inode
	sb->s_root = d_make_root(root_inode);
#else
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root)
		iput(root_inode);
#endif

	if (!sb->s_root) {
		ret = -ENOMEM;
		goto release;
	}


	ret = 0;
release:
	brelse(bh);

	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	ret = mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

	if (unlikely(IS_ERR(ret)))
		printk(KERN_ERR "Error mounting simplefs");
	else
		printk(KERN_INFO "simplefs is succesfully mounted on [%s]\n",
		       dev_name);

	return ret;
}

static void simplefs_kill_superblock(struct super_block *sb)
{
	struct simplefs_sb_info *sb_info = sb->s_fs_info;

	printk(KERN_INFO
	       "simplefs superblock is destroyed. Unmount succesful.\n");
	/* This is just a dummy function as of now. As our filesystem gets matured,
	 * we will do more meaningful operations here */

	kill_block_super(sb);
	//brelse(sb_info->bh);
	kfree(sb_info);
	return;
}

struct file_system_type simplefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = simplefs_kill_superblock,
	.fs_flags = FS_REQUIRES_DEV,
};

static int simplefs_init(void)
{
	int ret;

	printk("%s LINE = %d\n",__func__,__LINE__);
	sfs_inode_cachep = kmem_cache_create("sfs_inode_cache",
	                                     sizeof(struct simplefs_inode),
	                                     0,
	                                     (SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD),
	                                     NULL);
	
	sfs_entry_cachep = kmem_cache_create("sfs_entry_cachep",
										sizeof(struct simplefs_cache_entry),
										0,
										(SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD),
										NULL);

	if (!sfs_inode_cachep) {
		return -ENOMEM;
	}
	
	if (!sfs_entry_cachep) {
		return -ENOMEM;
	}

	ret = register_filesystem(&simplefs_fs_type);
	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully registered simplefs\n");
	else
		printk(KERN_ERR "Failed to register simplefs. Error:[%d]", ret);

	return ret;
}

static void simplefs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&simplefs_fs_type);
	kmem_cache_destroy(sfs_inode_cachep);

	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully unregistered simplefs\n");
	else
		printk(KERN_ERR "Failed to unregister simplefs. Error:[%d]",
		       ret);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sankar P");
