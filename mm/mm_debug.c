#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static struct proc_dir_entry *proc_root;
static struct proc_dir_entry *proc_entry;
#define USER_ROOT_DIR "mm_root"
#define USER_ENTRY "mm_entry"
#define INFO_LEN 16
char *info;
int mm_pid;
static int proc_mm_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n",info);
	return 0;
}

static int proc_mm_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_mm_show, NULL);//而调用single_open函数只需直接指定show的函数指针即可，个人猜测可能是在single_open函数中实现了seq_operations结构体 。如果使用seq_open则需要实现seq_operations。
}

static ssize_t proc_mm_write(struct file *file, const char __user *buffer,  size_t count, loff_t *f_pos)
{
	int ret = 0;
	if ( count > INFO_LEN)
		return -EFAULT;
	if(copy_from_user(info, buffer, count))
	{
		return -EFAULT;
	}
	ret = kstrtoint(info, 10, &mm_pid); //输入一个字符然后转换成10进制整形数
	printk("Debug : Process Pid : %d\n",mm_pid);
	return count;
}
static const struct file_operations mm_proc_fops = {
	.owner = THIS_MODULE,
	.open = proc_mm_open,
	.read = seq_read,
	.write = proc_mm_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mm_create_proc_entry(void)
{
	int error = 0;
	proc_root = proc_mkdir(USER_ROOT_DIR, NULL); //创建目录
	if (NULL==proc_root)
	{
		printk(KERN_ALERT"Create dir /proc/%s error!\n", USER_ROOT_DIR);
		return -ENOMEM;
	}
	printk(KERN_INFO"Create dir /proc/%s\n", USER_ROOT_DIR);

	// proc_entry =create_proc_entry(USER_ENTRY, 0666, proc_root);
	proc_entry = proc_create(USER_ENTRY, 0666, proc_root, &mm_proc_fops); //在proc_root下创建proc_entry.
	if (NULL ==proc_entry)
	{
		printk(KERN_ALERT"Create entry %s under /proc/%s error!\n", USER_ENTRY,USER_ROOT_DIR);
		error = -ENOMEM;
		goto err_out;

	}

//proc_entry->write_proc= mm_writeproc;
//proc_entry->read_proc = mm_readproc;
	printk(KERN_INFO"Create /proc/%s/%s\n", USER_ROOT_DIR,USER_ENTRY);

	return 0;
err_out:
//proc_entry->read_proc = NULL;
//proc_entry->write_proc= NULL;
	remove_proc_entry(USER_ENTRY, proc_root);
	remove_proc_entry(USER_ROOT_DIR, NULL);
	return error;
}
static int mmproc_init(void)
{
	int ret = 0;
	printk("mmproc_init\n");

	info = kmalloc(INFO_LEN * sizeof(char), GFP_KERNEL);
	if (!info)
	{
		ret = -ENOMEM;
		goto fail;
	}
	memset(info, 0, INFO_LEN);
	mm_create_proc_entry();
	return 0;
fail:
	return ret;
}

static void mmproc_exit(void)
{
	kfree(info);
	
	remove_proc_entry(USER_ENTRY, proc_root);
	remove_proc_entry(USER_ROOT_DIR, NULL);
}
MODULE_AUTHOR("mm");
MODULE_LICENSE("GPL");
module_init(mmproc_init);
module_exit(mmproc_exit);
