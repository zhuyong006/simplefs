
#include<linux/module.h>  

#include<linux/types.h>  

#include<linux/fs.h>  

#include<linux/errno.h>  

#include<linux/mm.h>  

#include<linux/sched.h>  

#include<linux/init.h>  

#include<linux/cdev.h>  

#include<asm/io.h>  

#include<asm/uaccess.h>  

#include<linux/poll.h>  

#include <linux/slab.h>

#include <linux/ioctl.h>

  

#define GLOBALFIFO_SIZE  10  /*全局fifo最大10字节*/  

#define FIFO_CLEAR 0X1       /*清0全局内存的长度*/  

#define GLOBALFIFO_MAJOR 100 /*主设备号*/  

  

static int globalfifo_major = GLOBALFIFO_MAJOR;  

  

/*globalfifo设备结构体*/  

struct globalfifo_dev{  

	struct cdev cdev;                   /*cdev结构体*/  

	unsigned int current_len;           /*fifo有效数据长度*/  

    unsigned char mem[GLOBALFIFO_SIZE]; /*全局内存*/  //内核的共享数据区

    struct semaphore sem;               /*并发控制用的信号量*/  

    wait_queue_head_t r_wait;           /*阻塞读用的等待队列,内核双向循环链表*/  

    wait_queue_head_t w_wait;           /*阻塞写用的等待队列头*/  

};  

  

struct globalfifo_dev *globalfifo_devp; /*设备结构体指针*/  

  

/*globalfifo读函数*/  //cat /dev/xxx

static ssize_t globalfifo_read(struct file *filp, 

	char __user *buf, size_t count, loff_t *ppos)  

{  

    int ret;  

    struct globalfifo_dev *dev = filp->private_data;  //private_data全局变量

    DECLARE_WAITQUEUE(wait, current);  //定义当前进程的等待队列wait，current指针指向当前在运行的进程

  

    down(&dev->sem);                     /*获得信号量*/  

    add_wait_queue(&dev->r_wait, &wait); /*加入读等待队列头 到内核*/ //把wait添加到等待队列头r_wait指向的等待队列链表中，并不代表已经睡眠了，还需要调度函数的调度

  

    /*等待FIFO 非空*/ //如果共享数据区mem的数据长度为0，就应该阻塞该进程

    if(dev->current_len == 0){  

        if(filp->f_flags & O_NONBLOCK){   /*如果进程为 非阻塞打开 设备文件*/  

            ret = -EAGAIN;  //再进行一次读操作

            goto out;  

        }  

        __set_current_state(TASK_INTERRUPTIBLE); /*改变进程状态为睡眠*/  

        up(&dev->sem);                           /*释放信号量*/  

  

        schedule();                              /*调度其他进程执行*/  //此时读进程才会真正的睡眠，直至被写进程唤醒。在睡眠途中，如果用户给读进程发送了信号，那么也会唤醒睡眠的进程

        if(signal_pending(current)){             /*如果是因为信号唤醒*/ //因为是调度出去，进程状态是浅度睡眠，唤醒它的有可能是信号 

            ret = -ERESTARTSYS;  //表示信号函数处理完毕后重新执行信号处理函数前的某个系统调用

            goto out2;  

        }  

        down(&dev->sem);  //加入信号量down和up防止多个进程同时访问共享数据mem

    }  

  

    /*拷贝到用户空间*/  

    if(count > dev->current_len)  //如果当前读的数据大于fifo的有效数据长度

        count = dev->current_len;  

    if(copy_to_user(buf, dev->mem, count)){  //参数：to from count，成功返回0，失败是返回还没有拷贝到用户空间的字节数

        ret = -EFAULT;  

        goto out;  

    }else{  

        memcpy(dev->mem, dev->mem + count, dev->current_len - count);/*数据前移*/  //memcpy将(dev->mem + count)开始的(dev->current_len - count)字节的数据移动到缓冲区最开始的地方

        dev->current_len -= count; /*有效数据长度减少*/  

        printk(KERN_INFO"read %ld bytes(s),current_len:%d\n",count, dev->current_len);  

  

        wake_up_interruptible(&dev->w_wait); /*唤醒写等待队列*/	//已经读完数据，就要唤醒写队列来进行写数据

        ret = count;  

    }  

out:  

    up(&dev->sem); /*释放信号量*/  

out2:  

    remove_wait_queue(&dev->w_wait, &wait); /*从属的等待队列头移除*/  

    set_current_state(TASK_RUNNING);  

    return ret;  

}  

  

/*globalfifo 写操作*/  //echo " " > /dev/xxx

static ssize_t globalfifo_write(struct file *filp, 

	const char __user *buf, size_t count, loff_t *ppos)  

{  

    struct globalfifo_dev *dev = filp->private_data;  

    int ret;  

    DECLARE_WAITQUEUE(wait, current);    /*定义等待队列*/    

    down(&dev->sem);                     /*获得信号量*/  

    add_wait_queue(&dev->w_wait, &wait); /*进入写等待队列头*/  

  

    /*等待FIFO非满*/  //如果共享数据区的数据长度等于fifo的大小，表示已经满了，就应该阻塞

    if(dev->current_len == GLOBALFIFO_SIZE){  

        if(filp->f_flags & O_NONBLOCK){   

			/*如果进程非阻塞打开的文件*/  

            ret = -EAGAIN;  

            goto out;  

        }  

  

        __set_current_state(TASK_INTERRUPTIBLE); /*改变进程状态为睡眠*/  

        up(&dev->sem);                     /*释放信号量*/  

  

        schedule();                         /*调度其他进程执行*/  

        if(signal_pending(current)){  

                                            /*如果是因为信号唤醒*/  

            ret = -ERESTARTSYS;  

            goto out2;  

        }  

        down(&dev->sem);                    /*获得信号量*/  

    }  

  

    /*从用户空间拷贝数据到内核空间*/  

    if(count > GLOBALFIFO_SIZE - dev->current_len){	//如果fifo的大小大于有效内存的长度，则下次再写  

        /*如果要拷贝的数据大于 剩余有效内存长度   

         *则 只拷贝最大 能装下的长度*/  

        count = GLOBALFIFO_SIZE - dev->current_len;  //count保留下次再写的数据大小

    }  

    if(copy_from_user(dev->mem + dev->current_len, buf, count)){  //参数to，from，count，to：(dev->mem) + (dev->current_len)，因为写入数据了，所以当前的共享数据区，要移位

        ret = -EFAULT;  

        goto out;  

    }else {  

        dev->current_len += count;  //有效数据有加当前的数据的长度

        printk(KERN_INFO"written %ld bytes(s), current_len: %d\n",count, dev->current_len);  

  

        wake_up_interruptible(&dev->r_wait); /*唤醒读等待队列*/ //写完数据，那肯定要唤醒读队列进行读啦 

        ret = count;  

    }  

    out:  

        up(&dev->sem); /*释放信号量*/  //释放信号量，读进程会因信号量被释放而唤醒

    out2:  

        remove_wait_queue(&dev->w_wait, &wait); /*从附属的等待队列头移除*/  

        set_current_state(TASK_RUNNING);  //进程处于可运行状态

        return ret;  

}  

   

   

/*ioctl 设备控制函数*/  

static long globalfifo_ioctl(struct file *filp,unsigned int cmd, unsigned long arg)

{  

    struct globalfifo_dev *dev = filp->private_data;/*获得设备结构体指针*/  

  

    switch(cmd){  

        case FIFO_CLEAR:  

            down(&dev->sem);                        /*获得信号量*/  

            dev->current_len = 0;  

            memset(dev->mem, 0, GLOBALFIFO_SIZE);  

            up(&dev->sem);                          /*释放信号量*/  

  

            printk(KERN_INFO"globalfifo is set to zero\n");  

            break;  

  

        default:  

            return -EINVAL;  

    }  

    return 0;  

}  

   

/*在驱动中的增加轮询操作*/  

static unsigned int globalfifo_poll(struct file *filp, poll_table *wait)  

{  

    unsigned int mask = 0;  

    struct globalfifo_dev *dev = filp->private_data;/*获得设备结构体指针*/  

  

    down(&dev->sem);  

    poll_wait(filp, &dev->r_wait, wait);  

    poll_wait(filp, &dev->w_wait, wait);  

  

    /*fifo非空*/  

    if(dev->current_len != 0){  

        mask |= POLLIN | POLLRDNORM; /*标示数据可以获得*/  

    }  

  

    /*fifo 非满*/  

    if(dev->current_len != GLOBALFIFO_SIZE){  

        mask |= POLLOUT | POLLWRNORM ; /*标示数据可以写入*/  

    }  

  

    up(&dev->sem);  

    return mask; /*返回驱动是否可读 或可写的 状态*/  

}  

  

/*文件打开函数*/  

int globalfifo_open(struct inode *inode, struct file *filp)  

{  

    /*让设备结构体作为设备的私有信息*/  

    filp->private_data = globalfifo_devp;  

    return 0;  

}  

  

/*文件释放函数*/  

int globalfifo_release(struct inode *inode, struct file *filp)  

{  

    return 0;  

}  

  

/*文件操作结构体*/  

static const struct file_operations globalfifo_fops = {  

    .owner = THIS_MODULE,  

    .read = globalfifo_read,  

    .write = globalfifo_write,  

    .unlocked_ioctl = globalfifo_ioctl,  

    .poll = globalfifo_poll,  

    .open = globalfifo_open,  

    .release = globalfifo_release,  

};  

  

/*初始化并注册cdev*/  

static void globalfifo_setup_cdev(struct globalfifo_dev *dev, int index)  

{  

    int err, devno = MKDEV(globalfifo_major, index);  

  

    cdev_init(&dev->cdev, &globalfifo_fops);  

    dev->cdev.owner = THIS_MODULE;  

    err = cdev_add(&dev->cdev, devno, 1);  

    if(err)  

        printk(KERN_NOTICE "Error %d adding gloabalfifo %d", err, index);  

}  

   

/*设备驱动模块加载函数*/  

int globalfifo_init(void)  

{  

    int ret;  

    dev_t devno = MKDEV(globalfifo_major, 0);  

  

    /*申请设备号*/  

    if(globalfifo_major)  

        ret = register_chrdev_region(devno, 1, "globalfifo");  

    else{/*动态申请设备号*/  

        ret = alloc_chrdev_region(&devno, 0, 1, "globalfifo");  

        globalfifo_major = MAJOR(devno);  

    }  

  

    if(ret < 0)  

        return ret;  

  

    /*动态申请设备结构体的内存*/  

    globalfifo_devp = kmalloc(sizeof(struct globalfifo_dev), GFP_KERNEL);  

    if(!globalfifo_devp){  

        ret = - ENOMEM;  

		goto fail_malloc;  

    }  

  

    memset(globalfifo_devp, 0, sizeof(struct globalfifo_dev));  

  

    globalfifo_setup_cdev(globalfifo_devp, 0);  

  

		sema_init(&globalfifo_devp->sem,1);             /*初始化信号量*/  

    init_waitqueue_head(&globalfifo_devp->r_wait);  /*初始化读等待队列头*/  

    init_waitqueue_head(&globalfifo_devp->w_wait);  /*初始化写等待队列头*/  

  

    return 0;  

  

fail_malloc: unregister_chrdev_region(devno, 1);  

             return ret;  

}  

   

void globalfifo_exit(void)  

{  

    cdev_del(&globalfifo_devp->cdev); /*注销cdev*/  

    kfree(globalfifo_devp); /*释放设备结构体内存*/  

    unregister_chrdev_region(MKDEV(globalfifo_major, 0), 1); /*释放设备号*/  

}  

   

MODULE_AUTHOR("54geeker");  

MODULE_LICENSE("Dual BSD/GPL");    

module_init(globalfifo_init);  

module_exit(globalfifo_exit);  
