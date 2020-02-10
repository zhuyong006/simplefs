#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/of.h>
#include <linux/completion.h>
#include <linux/mfd/core.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>


struct work_struct work_demo;
struct work_struct work_demo2;

struct workqueue_struct *workqueue_demo;

char * tmp_ptr = NULL;
/*
	1. "slub_debug" in /proc/cmdline
	2.  set .config file as bellow
		CONFIG_SLUB_DEBUG_ON=y
		CONFIG_SLUB_DEBUG=y
		CONFIG_SLUB_STATS=y
		CONFIG_SLUB=y
	3. aarch64-linux-gnu-gcc -o slabinfo slabinfo.c --- /tools/vm/
	4. slabinfo -v
		* TWO CONDITION WILL CHECK MEM BUG *
		4.1. CHECK MEM BUG WHILE FREE MEM;
		4.2. SLABINFO -V WILL FORCE CHECK MEM BUG
		
*/
//#define MM_DEBUG


/*
	CONFIG_DEBUG_ATOMIC_SLEEP=y
*/

//#define SPIN_LOCK_SLEEP
#ifdef SPIN_LOCK_SLEEP
static spinlock_t spinlock;
#endif

/*
	1. set .config file as bellow
	   CONFIG_HAVE_DEBUG_KMEMLEAK=y
        CONFIG_DEBUG_KMEMLEAK=y
	   CONFIG_DEBUG_KMEMLEAK_EARLY_LOG_SIZE=400
	2. "kmemleak=on" in /proc/cmdline
*/
#define MM_LEAK_DEBUG
static void work_demo_func(struct work_struct *work)
{
	printk("%s ,cpu id = %d,taskname = %s\n",
		__func__,raw_smp_processor_id(),current->comm);
	mdelay(1000*10);
}

static int workqueue_proc_show(struct seq_file *m, void *v)
{

	printk("%s ,cpu id = %d\n",__func__,raw_smp_processor_id());

	//queue_work(workqueue_demo,&work_demo);
#if 0	
	schedule_work(&work_demo);
#endif

#ifdef SPIN_LOCK_SLEEP	
	spin_lock(&spinlock);
	msleep(10);
	spin_unlock(&spinlock);
#endif

#ifdef MM_DEBUG
	char *mm = 	kmalloc(32,GFP_KERNEL);
	if(mm)
	{
		mm[65] = 1;
		kfree(mm);
		printk("free mm buf\n");

	}
#endif

	return 0;
}

static int workqueue_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, workqueue_proc_show, NULL);
}

static ssize_t workqueue_proc_store(struct file *file, const char __user *buffer,
			      size_t count, loff_t *ppos)
{
#if 0
	char  buf[count];
	
	copy_from_user(buf, buffer, count);

	if(buf[0] == '1')
	{
		printk("%s ,work_demo,cpu id = %d\n",__func__,raw_smp_processor_id());
		queue_work(workqueue_demo,&work_demo);
		printk("queue work_demo end\n");
	}
	else if(buf[0] == '2')
	{
		printk("%s ,work_demo2,cpu id = %d\n",__func__,raw_smp_processor_id());
		queue_work(workqueue_demo,&work_demo2);
	}
#endif

#ifdef MM_LEAK_DEBUG
	int i = 0;
	do
	{
		char *mm =	kmalloc(8*1024, GFP_KERNEL);
		printk("mem leak,ptr = %p\n",mm);
		if(mm)
		{
			memset(mm,0x0,8*1024);
			mm = kmalloc(8*1024, GFP_KERNEL);
			printk("mem leak,ptr = %p\n",mm);
			kfree(mm);
		}
		i++;
	}while(i<1024/8);
#endif

	
	return count;
}


static const struct file_operations workqueue_proc_fops = {
	.open		= workqueue_proc_open,
	.read		= seq_read,
	.write      = workqueue_proc_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static int __init workqueue_init(void)
{
	
	INIT_WORK(&work_demo, work_demo_func);
	INIT_WORK(&work_demo2, work_demo_func);
	//workqueue_demo = create_workqueue("workqueue demo");
	workqueue_demo = alloc_workqueue("workqueue demo", 0, 2);
#ifdef SPIN_LOCK_SLEEP		
	spin_lock_init(&spinlock);
#endif
	proc_create("workqueue", 0, NULL, &workqueue_proc_fops);

	return 0;
}

static void __exit workqueue_exit(void)
{
	return ;
}

module_init(workqueue_init);
module_exit(workqueue_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jon");
MODULE_ALIAS("platform");
MODULE_DESCRIPTION("workqueue demo driver");

