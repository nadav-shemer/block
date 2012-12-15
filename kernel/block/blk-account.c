/* bp_account_record() and friends
 *
 * Copyright (C) 2012 by Nadav Shemer <nadav.shemer@gmail.com>
 *
 * This file is released under the General Public License version 2 (GPL v2).
 *
 * This file may be redistributed under the terms of the GNU Public
 * License.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/poll.h>
#include <linux/pagemap.h>
#include <linux/hugetlb.h>
/* NADAV bp */
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/proc_fs.h>
#include <linux/ratelimit.h>

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

struct bp_request_line {
	unsigned long block;		/* 8  (8) */
	int size;			/* 4  (12) */
	unsigned int devid;		/* 4  (16) */
};

struct bp_account_line {
	unsigned long jiffies;		/* 8  (8) */
	unsigned long i_ino;		/* 8  (16) */
	unsigned long block;		/* 8  (24) */
	unsigned int devid;		/* 4  (28) */
	unsigned int pgdevid;		/* 4  (32) */
	int pgindex;			/* 4  (36) */
	int size;			/* 4  (40) */
	int pid;			/* 4  (44) */
	int tgid;			/* 4  (48) */
	int reason;			/* 4  (52) */
	char comm[TASK_COMM_LEN];	/* 16 (68) */
	char devname[BDEVNAME_SIZE];	/* 32 (100) */
} __packed;

static DEFINE_PER_CPU(int, bp_exiting);

/* Exclusive producer (CPU #n), Exclusive reader (by mutex). All synchronization is lazy (except one barrier in writer) */
struct bp_buffer {
	int rptr ____cacheline_aligned;
	int wptr ____cacheline_aligned;
};
static DEFINE_PER_CPU(struct bp_buffer, bp_account_buffer);
static DEFINE_PER_CPU(struct bp_account_line *, bp_line_buffer);

#define BP_SHIFT	(15)
#define BP_SIZE		(1 << BP_SHIFT)
#define BP_MASK		(BP_SIZE-1)
#define BP_INC(x)	(((x)+1)&BP_MASK)

/* After MIN_FILL lines, we wake the thread up immediately */
#define MIN_FILL	(BP_SIZE >> 1)

static DEFINE_PER_CPU(struct mutex, bp_mutex);

static DEFINE_PER_CPU(wait_queue_head_t, bp_user_wait);
static DEFINE_PER_CPU(struct proc_dir_entry *, bp_proc_kbpd);

struct bp_statistics_thread {
	wait_queue_head_t wait;
	struct task_struct *tsk;
	struct completion start_done;
};
static struct bp_statistics_thread bp_statistics_thread = {
	.wait = __WAIT_QUEUE_HEAD_INITIALIZER(bp_statistics_thread.wait),
	.tsk = NULL,
	.start_done = {
		.done = 0,
		.wait = __WAIT_QUEUE_HEAD_INITIALIZER(bp_statistics_thread.start_done.wait),
	},
};

static unsigned long bp_last_print = 0;
#define bp_printk(fmt, args...)		do { if (printk_timed_ratelimit(&bp_last_print, HZ >> 2)) printk(fmt, ##args); } while (0)
#ifndef STRINGIFYFY
#define STRINGIFYFY(i) #i
#endif
#ifndef STRINGIFY
#define STRINGIFY(i) STRINGIFYFY(i)
#endif
#define bp_dprintk(fmt, args...)	do { 						\
		static DEFINE_RATELIMIT_STATE(____ratelimit_state, 20 * HZ, 5);		\
		if (___ratelimit(&____ratelimit_state, __FILE__ STRINGIFY(__LINE__)))	\
			printk(fmt, ##args);						\
} while (0)
#define dbp_printk	bp_dprintk
//#define dbp_printk(fmt, args...)	do { } while (0)

static inline int __bp_fill(int wptr, int rptr) {
	return ((wptr + BP_SIZE - rptr) & BP_MASK);
}

static inline int _bp_fill(struct bp_buffer *bp_buffer) {
	return __bp_fill(bp_buffer->wptr, bp_buffer->rptr);
}

static void bp_account_record(struct block_device *bdev, unsigned int pgdevid, unsigned long i_ino, int index, unsigned long block, int size, int reason)
{
	struct bp_buffer *bp_buffer;
	int wptr;
	struct bp_account_line *buf;

	BUILD_BUG_ON(sizeof(struct bp_account_line) != 100);

	BUG_ON(preemptible());

	if (unlikely(__get_cpu_var(bp_exiting) == 1))
		return;
	bp_buffer = this_cpu_ptr(&bp_account_buffer);
	wptr = bp_buffer->wptr;
	if (unlikely(BP_INC(wptr) == bp_buffer->rptr))
		return;
	buf = __get_cpu_var(bp_line_buffer);
	buf[wptr].jiffies = jiffies;
	buf[wptr].i_ino = i_ino;
	buf[wptr].block = block;
	buf[wptr].devid = (bdev)?bdev->bd_dev:0;
	buf[wptr].pgdevid = pgdevid;
	buf[wptr].pgindex = index;
	buf[wptr].size = size;
	buf[wptr].reason = reason;
	if (current) {
		buf[wptr].pid = task_pid_nr(current);
		buf[wptr].tgid = task_tgid_nr(current);
		strncpy(buf[wptr].comm, current->comm, TASK_COMM_LEN);
	} else {
		buf[wptr].pid = 0;
		buf[wptr].tgid = 0;
		strncpy(buf[wptr].comm, "NoTask", TASK_COMM_LEN);
	}
	if (bdev) {
		memset(buf[wptr].devname, 0, BDEVNAME_SIZE);
		bdevname(bdev, buf[wptr].devname);
	} else
		strncpy(buf[wptr].devname, "NODEVNAME", BDEVNAME_SIZE);
	/* The data must be visible before incrementing the write pointer */
	smp_wmb();
	bp_buffer->wptr = BP_INC(wptr);
	if (waitqueue_active(this_cpu_ptr(&bp_user_wait)))
		wake_up_interruptible(this_cpu_ptr(&bp_user_wait));
}

#if 0
static void bp_account_record(struct block_device *bdev, unsigned long block, int size, int reason)
{
	_bp_account_record(bdev, 0, 0, 0, block, size, reason);
}
static void bp_account_page_block(struct block_device *bdev, unsigned long block, int size, unsigned int devid, unsigned long ino, int index, int reason)
{
	_bp_account_record(bdev, devid, ino, index, block, size, reason);
}
static void bp_account_page(unsigned int devid, unsigned long ino, int index, int reason)
{
	_bp_account_record(NULL, devid, ino, index, 0, 0, reason);
}
#endif

static inline int _bp_exiting(void) {
	int ret;
	preempt_disable();
	ret = __get_cpu_var(bp_exiting);
	preempt_enable();
	return ret;
}

static int bp_statistics_thread_worker(void *arg)
{
	unsigned long next_time = jiffies + (HZ * 20);

	printk(KERN_DEBUG "Starting bp_statistics_global thread pid %d\n", task_pid_nr(current));

	complete(&bp_statistics_thread.start_done);

	set_current_state(TASK_INTERRUPTIBLE);

	set_freezable();

	while (!kthread_should_stop()) {
		int cpu;
		int datums = 0;

		preempt_disable();
		if (__get_cpu_var(bp_exiting) == 1) {
			preempt_enable();
			printk(KERN_ERR "Exiting - break statistics_thread\n");
			break;
		}
		preempt_enable();

		if (jiffies < next_time) {
			wait_event_interruptible_timeout(bp_statistics_thread.wait,
							 kthread_should_stop(),
							 next_time - jiffies);
			try_to_freeze();
			continue;
		}

		__set_current_state(TASK_RUNNING);

		next_time = jiffies + (HZ * 20);

		for_each_online_cpu(cpu) {
			struct bp_buffer *bp_buffer = &per_cpu(bp_account_buffer, cpu);
			datums += _bp_fill(bp_buffer);
		}
		printk(KERN_INFO "BP %lu bytes waiting for user in %d buffers\n", datums * sizeof(struct bp_account_line), datums);

		if (need_resched())
			cond_resched();

		try_to_freeze();

		set_current_state(TASK_INTERRUPTIBLE);
	}

	printk(KERN_ERR "Stopping %s\n", bp_statistics_thread.tsk->comm);

	/* Wait for kthread_stop */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	__set_current_state(TASK_RUNNING);

	return 0;
}

static DEFINE_PER_CPU(int, bp_file_busy);

static int bp_global_open(struct inode *inode, struct file *file)
{
	int cpu = (long)(PDE(inode)->data);
	if (try_module_get(THIS_MODULE)) {
		if (mutex_trylock(&per_cpu(bp_mutex, cpu))) {
			if (per_cpu(bp_file_busy, cpu)) {
				bp_dprintk(KERN_ERR "open returns busy\n");
				mutex_unlock(&per_cpu(bp_mutex, cpu));
				return -EBUSY;
			}
			per_cpu(bp_file_busy, cpu) = 1;
			mutex_unlock(&per_cpu(bp_mutex, cpu));
			file->private_data = (void *)((long)cpu);
			return 0;
		}
		module_put(THIS_MODULE);
		bp_dprintk(KERN_ERR "open returns busy (mutex is contended)\n");
		return -EBUSY;
	} else {
		bp_dprintk(KERN_ERR "try_module_get failed - open returns error\n");
		return -EIO;
	}
}

static int bp_global_close(struct inode* inode, struct file* filp)
{
	int cpu = (long)(PDE(inode)->data);
	BUG_ON(filp->private_data != (void *)((long)cpu));
	BUG_ON(!per_cpu(bp_file_busy, cpu));
	per_cpu(bp_file_busy, cpu) = 0;
	module_put(THIS_MODULE);
	return 0;
}

static inline int bp_empty(int cpu)
{
	struct bp_buffer *bp_buffer = &per_cpu(bp_account_buffer, cpu);
	return (bp_buffer->rptr == bp_buffer->wptr);
}

static unsigned int bp_global_poll(struct file *filp, poll_table *wait)
{
	int cpu = (long)(filp->private_data);
	poll_wait(filp, &per_cpu(bp_user_wait, cpu), wait);

	if (unlikely(per_cpu(bp_exiting, cpu) == 1)) {
		return POLLIN | POLLRDNORM | POLLERR;
	}
	if (bp_empty(cpu))
		return 0;
	return POLLIN | POLLRDNORM;
}

static inline size_t iov_total(const struct iovec *iv, unsigned long count)
{
	unsigned long i;
	size_t len;

	for (i = 0, len = 0; i < count; i++)
		len += iv[i].iov_len;

	return len;
}

static ssize_t bp_global_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
//	int cpu = (long)(filp->private_data);
	struct block_device *bdev;
	struct bp_request_line bp;
	struct buffer_head *bh;
	int read_size;
	int read = 0;

	if (unlikely(len == 0))
		return 0;

	dbp_printk(KERN_INFO "Write\n");

	if (len != sizeof(struct bp_request_line)) {
		bp_printk(KERN_ERR "Bad write size\n");
		return -EINVAL;
	}
	if (copy_from_user(&bp, (void *)buf, len)) {
		bp_printk(KERN_ERR "Efault\n");
		return -EFAULT;
	}

	dbp_printk(KERN_INFO "Looking up device %x\n", bp.devid);
	bdev = bdget(bp.devid);
	if (IS_ERR(bdev)) {
		bp_dprintk(KERN_ERR "Can't get device %x err %ld\n", bp.devid, PTR_ERR(bdev));
		return -ENODEV;
	}
	if (bdev == NULL) {
		bp_dprintk(KERN_ERR "Can't get device %x\n", bp.devid);
		return -ENODEV;
	}
	read_size = bdev_logical_block_size(bdev);
	if (read_size != 512) {
		bp_dprintk(KERN_ERR "Logical read size is odd (%d)\n", read_size);
		bdput(bdev);
		return -EINVAL;
	}
	/* The pagecache uses (1<<PAGE_CACHE_SHIFT) sized blocks (one page) */
	if (bdev->bd_inode->i_blkbits != PAGE_CACHE_SHIFT) {
		bp_dprintk(KERN_ERR "Device block size (%d) different from page cache size (%d)\n", 1 << bdev->bd_inode->i_blkbits, 1 << PAGE_CACHE_SHIFT);
		bdput(bdev);
		return -EINVAL;
	}
	bp.size = (bp.size + ((1 << PAGE_CACHE_SHIFT)-1)) >> PAGE_CACHE_SHIFT; /* Round up */
	dbp_printk(KERN_INFO "Reading %lu bytes\n", bp.size * PAGE_CACHE_SIZE);
	while (read < bp.size) {
		bh = __bread(bdev, bp.block, PAGE_CACHE_SIZE);
		if (bh) {
			lock_buffer(bh);
			if (!buffer_uptodate(bh)) {
				bp_dprintk(KERN_INFO "Buffer %d not up to date\n", (int)bp.block);
			}
			unlock_buffer(bh);
			brelse(bh);
			bp.block++;
			read++;
		} else {
			bp_dprintk(KERN_ERR "Failed to get buffer head for %d\n", (int)bp.block);
			bdput(bdev);
			return -EAGAIN;
		}
		if (need_resched())
			cond_resched();
	}
	bdput(bdev);
	return len;
}

static ssize_t bp_global_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	int cpu = (long)(filp->private_data);
	char __user *out = buf;
	ssize_t read = 0;
	int res = 0;
	struct bp_buffer *bp_buffer = &per_cpu(bp_account_buffer, cpu);
	int rptr = bp_buffer->rptr; /* We're exclusive writers by mutex lock */

	if (unlikely(len == 0))
		return 0;
	if (unlikely(len < sizeof(struct bp_account_line)))
		return -ETOOSMALL;
	len -= (len % sizeof(struct bp_account_line));

	while (len) {
		int to_copy;
		to_copy = __bp_fill(bp_buffer->wptr, rptr);
		if (to_copy == 0) {
			if (filp->f_flags & O_NONBLOCK) {
				res = -EAGAIN;
				break;
			}
			if (wait_event_interruptible(per_cpu(bp_user_wait, cpu), !bp_empty(cpu))) {
				res = -EINTR;
				break;
			}
			to_copy = __bp_fill(bp_buffer->wptr, rptr);
		}
		/* Overflow */
		if (((rptr + to_copy) & BP_MASK) != (rptr + to_copy))
			to_copy = BP_SIZE-rptr;
		if ((to_copy * sizeof(struct bp_account_line)) > len)
			to_copy = len / sizeof(struct bp_account_line);
		BUG_ON( to_copy == 0 );
		if (copy_to_user(out, &(per_cpu(bp_line_buffer, cpu)[rptr]), to_copy * sizeof(struct bp_account_line))) {
			res = -EFAULT;
			break;
		}
		/* If overflow before, rptr will now be 0 */
		rptr = (rptr + to_copy) & BP_MASK;
		bp_buffer->rptr = rptr;
		read += to_copy * sizeof(struct bp_account_line);
		out += to_copy * sizeof(struct bp_account_line);
		len -= to_copy * sizeof(struct bp_account_line);
	}
	if (read)
		return read;
	return res;
}

static struct proc_dir_entry *bp_proc_dir;
static const struct file_operations bp_global_fops = {
	.owner    = THIS_MODULE,
	.llseek   = no_llseek,
	.open     = bp_global_open,
	.release  = bp_global_close,
	.read     = bp_global_read,
	.write    = bp_global_write,
	.poll     = bp_global_poll,
};

static int __init bp_create_global_statistics_thread(void)
{
	struct task_struct *p;

	p = kthread_create_on_node(bp_statistics_thread_worker,
				   &bp_statistics_thread,
				   -1,
				   "kbp_statd");
	if (IS_ERR(p)) {
		printk(KERN_ERR "kernel_thread() failed for global statistics thread\n");
		return PTR_ERR(p);
	}
	bp_statistics_thread.tsk = p;

	/* Low priority */
	set_user_nice(p, 19);

	wake_up_process(p);
	wait_for_completion(&bp_statistics_thread.start_done);

	return 0;
}

typedef void (bp_hook_fn) (struct block_device *bdev, unsigned int pgdevid, unsigned long i_ino, int index, unsigned long block, int size, int reason);
extern int bp_hook_install(bp_hook_fn *fn);
extern int bp_hook_uninstall(bp_hook_fn *fn);

#define BP_PROC_DIR "bp"
static void __exit bp_account_exit(void)
{
	int i;

	for_each_possible_cpu(i) {
		char proc_name[32];
		per_cpu(bp_exiting, i) = 1;
		/* wake_up contains barriers - the process will see the bp_exiting set above */
		wake_up_interruptible(&per_cpu(bp_user_wait, i));
		sprintf(proc_name, "kbpd%d", i);
		remove_proc_entry(proc_name, bp_proc_dir);
	}
	remove_proc_entry(BP_PROC_DIR, NULL);

	kthread_stop(bp_statistics_thread.tsk);

	/* after bp_hook_uninstall, no new calls to bp_account_record will be made. It might still be called while running bp_hook_uninstall */
	if (bp_hook_uninstall(bp_account_record)) {
		printk(KERN_WARNING "BP: exit - hook already not installed\n");
	}

	/* Now we can release everything */
	for_each_possible_cpu(i) {
		kfree(per_cpu(bp_line_buffer, i));
	}
}

static int __init bp_account_init(void)
{
	int i;
	int err;

	for_each_possible_cpu(i) {
		mutex_init(&per_cpu(bp_mutex, i));
		init_waitqueue_head(&per_cpu(bp_user_wait, i));
		per_cpu(bp_file_busy, i) = 0;
		per_cpu(bp_exiting, i) = 0;
		per_cpu(bp_account_buffer, i).rptr = 0;
		per_cpu(bp_account_buffer, i).wptr = 0;
		per_cpu(bp_line_buffer, i) = kmalloc(sizeof(struct bp_account_line) * BP_SIZE, GFP_KERNEL);
		if (per_cpu(bp_line_buffer, i) == NULL) {
			int j;
			for_each_possible_cpu(j) {
				if (j == i)
					break;
				kfree(per_cpu(bp_line_buffer, j));
			}
			return -ENOMEM;
		}
	}

	bp_proc_dir = proc_mkdir(BP_PROC_DIR, NULL);
	if (!bp_proc_dir) {
		err = -EIO;
		goto free_all_buffers;
	}

	for_each_possible_cpu(i) {
		char proc_name[32];
		sprintf(proc_name, "kbpd%d", i);
		per_cpu(bp_proc_kbpd, i) = proc_create_data(proc_name, 0600, bp_proc_dir,
				      &bp_global_fops, (void *)((long)i));
		if (per_cpu(bp_proc_kbpd, i) == NULL) {
			int j;
			printk(KERN_ERR "cannot create kbpd procfs entry\n");
			for_each_possible_cpu(j) {
				char old_proc_name[32];
				if (j == i)
					break;
				sprintf(old_proc_name, "kbpd%d", j);
				remove_proc_entry(old_proc_name, bp_proc_dir);
			}
			err = -EIO;
			goto remove_bp_proc;
		}
	}

	err = bp_create_global_statistics_thread();
	if (err) {
		printk(KERN_WARNING "BP: Cannot create global statistics thread (%d)\n",
			   err);
		goto remove_kbpd_proc;
	}

	if (bp_hook_install(bp_account_record)) {
		printk(KERN_WARNING "BP: Cannot install hook\n");
		goto stop_global;
	}
	return 0;

stop_global:
	kthread_stop(bp_statistics_thread.tsk);
remove_kbpd_proc:
	for_each_possible_cpu(i) {
		char proc_name[32];
		sprintf(proc_name, "kbpd%d", i);
		remove_proc_entry(proc_name, bp_proc_dir);
	}
remove_bp_proc:
	remove_proc_entry(BP_PROC_DIR, NULL);
free_all_buffers:
	for_each_possible_cpu(i) {
		kfree(per_cpu(bp_line_buffer, i));
	}
	return err;
}

module_init(bp_account_init);
module_exit(bp_account_exit);
MODULE_LICENSE("GPL");
