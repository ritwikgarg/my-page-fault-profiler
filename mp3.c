#include "linux/gfp.h"
#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>

#include "mp3_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ritwik Garg <ritwikg3@illinois.edu>");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 0
#define WRITE_BUF_SIZE 128
#define BUF_SIZE 4096
#define SAMPLE_PERIOD msecs_to_jiffies(50)
#define PROFILER_BUFFER_PAGES    128
#define PROFILER_BUFFER_SIZE     (PROFILER_BUFFER_PAGES * PAGE_SIZE)
#define MAX_SAMPLES              12000
#define MP3_MAJOR 423
#define MP3_MINOR 0

/*
 * Protects the registered-task list and all decisions derived from it,
 * such as whether sampling should continue.
 */
static DEFINE_MUTEX(linked_list_lock);

/* Struct to denote each registered task */
struct mp3_task_struct {
   pid_t pid;
   struct list_head list;
};

/*
 * Raw sample format:
 *   (jiffies, soft_faults, hard_faults, cpu time in jiffies)
 */
struct profiler_sample {
	unsigned long jiffies;
	unsigned long soft_faults;
	unsigned long hard_faults;
	unsigned long cpu_util;
};

static void *profiler_buf;
static struct profiler_sample *profiler_samples;
static unsigned long sample_count;

/* Sampling is driven by a delayed work item that reschedules itself. */
static struct workqueue_struct *mp3_wq;
static struct delayed_work sample_work;

/* Character-device state used for mmap() of the profiler buffer. */
static dev_t mp3_dev = MKDEV(MP3_MAJOR, MP3_MINOR);
static struct cdev mp3_cdev;

LIST_HEAD(task_list);

/** 
 * find_task() - return the registered node for @pid
 * @pid: PID to search for
 *
 * Caller must hold linked_list_lock.
 */
static struct mp3_task_struct *find_task(pid_t pid) {
	struct mp3_task_struct *task;

	list_for_each_entry(task, &task_list, list) {
		if (task->pid == pid)
		return task;
	}
	return NULL;
};

/*
 * mp3_dev_open() and mp3_dev_release() are intentionally empty.
 * The device exists only to provide an mmap() entry point for the monitor.
 */
static int _mp3_dev_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int _mp3_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

/**
 * mp3_dev_mmap() - map the vmalloc'd profiler buffer into user space
 * @file: backing file for the character device
 * @vma:  user VMA being established
 *
 * The buffer is vmalloc'd, so we must translate each virtual page to a PFN
 * with vmalloc_to_pfn() and remap it one page at a time.
 */
static int _mp3_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long user_virt = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset;

	if (!profiler_buf)
		return -EINVAL;

	if (size > PROFILER_BUFFER_SIZE)
		return -EINVAL;

	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		unsigned long pfn;

		pfn = vmalloc_to_pfn((char *)profiler_buf + offset);

		if (remap_pfn_range(vma,
							user_virt + offset,
							pfn,
							PAGE_SIZE,
							vma->vm_page_prot))
			return -EAGAIN;
	}

	return 0;
}

static const struct file_operations mp3_dev_fops = {
	.open = _mp3_dev_open,
	.release = _mp3_dev_release,
	.mmap = _mp3_dev_mmap,
};

/*
 * setup_char_device() - register the character device used by monitor mmap()
 *
 * Requires major 423, minor 0.
 */
static int setup_char_device(void)
{
	int ret;

	ret = register_chrdev_region(mp3_dev, 1, "mp3");
	if (ret)
		return ret;

	cdev_init(&mp3_cdev, &mp3_dev_fops);
	mp3_cdev.owner = THIS_MODULE;

	ret = cdev_add(&mp3_cdev, mp3_dev, 1);
	if (ret) {
		unregister_chrdev_region(mp3_dev, 1);
		return ret;
	}

	return 0;
}

static void cleanup_char_device(void)
{
	cdev_del(&mp3_cdev);
	unregister_chrdev_region(mp3_dev, 1);
}


/*
 * Marks buffer pages as reserved.
 * We do this once at allocation time and clear the bits on teardown.
 */
static void mark_profiler_buffer_reserved(void)
{
	unsigned long addr;

	for (addr = (unsigned long)profiler_buf;
		addr < (unsigned long)profiler_buf + PROFILER_BUFFER_SIZE;
		addr += PAGE_SIZE) {
		SetPageReserved(vmalloc_to_page((void *)addr));
	}
}

static void unmark_profiler_buffer_reserved(void)
{
	unsigned long addr;

	for (addr = (unsigned long)profiler_buf;
		addr < (unsigned long)profiler_buf + PROFILER_BUFFER_SIZE;
		addr += PAGE_SIZE) {
		ClearPageReserved(vmalloc_to_page((void *)addr));
	}
}

/**
 * reset_profiler_buffer() - prepare the shared buffer for a fresh run
 *
 * The monitor expects unused entries to be initialized to -1. We also reset
 * sample_count so a new experiment starts writing at the beginning.
 */
static void reset_profiler_buffer(void)
{
	memset(profiler_buf, 0xFF, PROFILER_BUFFER_SIZE); /* initialize to -1 */
	sample_count = 0;
}

/*
 * start_sampling() - begin a fresh sampling run
 *
 * We reset the shared buffer here so each new set of registered tasks sees
 * a clean sample stream rather than stale data from an earlier experiment.
 */
static void start_sampling(void)
{
	reset_profiler_buffer();
	queue_delayed_work(mp3_wq, &sample_work, 0);
}

/*
 * stop_sampling() - stop the delayed worker
 *
 * cancel_delayed_work_sync() may sleep while waiting for an in-flight worker
 * to finish, so callers must not hold linked_list_lock across this call.
 */
static void stop_sampling(void)
{ 
	cancel_delayed_work_sync(&sample_work); 
}


/** 
 * append_sample() - append one aggregate sample to the shared buffer
 * @now:  current jiffies timestamp
 * @soft: summed soft faults for this interval
 * @hard: summed hard faults for this interval
 * @cpu:  summed CPU time in jiffies for this interval
 *
 * The buffer is treated as a bounded queue. Once it fills, we stop
 * appending new samples.
 */
static void append_sample(unsigned long now,
           unsigned long soft,
           unsigned long hard,
           unsigned long cpu)
{
   struct profiler_sample *s;

	if (sample_count >= MAX_SAMPLES)
		return;

	s = &profiler_samples[sample_count];

	s->soft_faults = soft;
	s->hard_faults = hard;
	s->cpu_util = cpu;
	s->jiffies = now;

	sample_count++;
}

/** 
 * sample_work_fn() - delayed sampling worker
 *
 * Every 50 ms, sum soft faults, hard faults, and raw CPU time across all
 * registered processes and append one aggregate sample to the shared buffer.
 * If the list is still non-empty, reschedule the worker for the next period.
 */
static void sample_work_fn(struct work_struct *work)
{
	struct mp3_task_struct *entry;
	unsigned long soft = 0;
	unsigned long hard = 0;
	unsigned long cpu = 0;
	bool need_requeue = false;

	mutex_lock(&linked_list_lock);

	/* Only sample while there is at least one registered task. */
	if (!list_empty(&task_list)) {
		unsigned long min_flt;
		unsigned long maj_flt;
		unsigned long utime;
		unsigned long stime;

		need_requeue = true;

		list_for_each_entry(entry, &task_list, list) {
			min_flt = 0;
			maj_flt = 0;
			utime = 0;
			stime = 0;

			/*
			 * get_cpu_use() returns the soft faults, hard faults,
			 * and CPU time accumulated since the previous call
			 * for this PID, then resets those counters.
			 */
			if (get_cpu_use(entry->pid,
					&min_flt,
					&maj_flt,
					&utime,
					&stime) == 0) {
				soft += min_flt;
				hard += maj_flt;
				cpu += nsecs_to_jiffies(utime) + nsecs_to_jiffies(stime);
			}
			#if DEBUG
			else {
				printk(KERN_INFO "mp3: failed to sample pid %d\n",
				       entry->pid);
			}
			#endif
		}

		/* Save one aggregated sample for this 50 ms interval. */
		append_sample(jiffies, soft, hard, cpu);
	}

	mutex_unlock(&linked_list_lock);

	/* Run again after 50 ms if there are still registered tasks. */
	if (need_requeue)
		queue_delayed_work(mp3_wq, &sample_work, SAMPLE_PERIOD);
}


/*
 * alloc_profiler_buffer() - allocate and initialize the shared sample buffer
 */
static int alloc_profiler_buffer(void)
{
	profiler_buf = vmalloc(PROFILER_BUFFER_SIZE);
	if (!profiler_buf)
		return -ENOMEM;

	profiler_samples = (struct profiler_sample *)profiler_buf;

	reset_profiler_buffer();
	mark_profiler_buffer_reserved();
	return 0;
}

static void free_profiler_buffer(void)
{
	if (!profiler_buf)
		return;

	unmark_profiler_buffer_reserved();
	vfree(profiler_buf);
	profiler_buf = NULL;
	profiler_samples = NULL;
	sample_count = 0;
}


/** 
 * handle_register_locked() - register a PID for profiling
 * @kernel_buf: procfs write buffer containing "R <PID>"
 * @need_start: set true when the task list transitions empty -> non-empty
 *
 * Caller must hold linked_list_lock.
 */
static int handle_register_locked(char *kernel_buf, bool *need_start) {
	pid_t pid;
	struct mp3_task_struct *node;
	int ret = 0;
   *need_start = false;

   /* Parse the input in the kernel buffer */
	if (sscanf(kernel_buf, "R %d", &pid) != 1) {
		return -EINVAL;
	}

   /* Check if input is valid */
	if (pid <= 0)
		return -EINVAL;

   /* Check for duplicate PID registration */
	if(find_task(pid)) 
		return -EINVAL;

	if (!find_task_by_pid(pid))
		return -EINVAL;

   /* Allocate memory for new node */
	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return -ENOMEM;
	}

	/* Initialize node fields */
	node->pid = pid;

	/* Insert node into process linked list */
	INIT_LIST_HEAD(&node->list);
	list_add(&node->list, &task_list);
	
   /* After insertion, singular means this is the first registered task. */
	if (list_is_singular(&task_list))
		*need_start = true;

	#if DEBUG
	printk(KERN_INFO "Successfully registered PID: %d\n", node->pid);
	#endif

	return ret;
}

/** 
 * handle_unregister_locked() - unregister a PID from profiling
 * @kernel_buf: procfs write buffer containing "U <PID>"
 * @need_stop:  set true when the task list transitions non-empty -> empty
 *
 * Caller must hold linked_list_lock.
 */
static int handle_unregister_locked(char *kernel_buf, bool *need_stop) {
	pid_t pid;
	struct mp3_task_struct *node;
   *need_stop = false;

   /* Parse the input in the kernel buffer */
	if(sscanf(kernel_buf, "U %d", &pid) != 1) {
		return -EINVAL;
	}

	node = find_task(pid);
	if (!node) {
		return -EINVAL;
	}

	list_del(&node->list);
	kfree(node);

	/*
	 * If the list is now empty, sampling must be stopped once the caller
	 * drops the mutex.
	 */
	if (list_empty(&task_list))
      *need_stop = true;

	#if DEBUG
	printk(KERN_INFO "Successfully de-registered PID: %d\n", pid);
	#endif
	return 0;
}

/**
* file_read - outputs the task linked list to user space
* @file: file that is to be read from
* @user_buf: buffer in user space to be copied into
* @count: no. of bytes to be read
* @ppos: file offset
*
* Format per line:
* <PID>
*/
static ssize_t file_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {
	ssize_t ret;
	size_t len = 0;
	struct mp3_task_struct *entry;
	char *local_buf;

	local_buf = kmalloc(BUF_SIZE, GFP_KERNEL);
	if (!local_buf)
		return -ENOMEM;

	
	/* Clear local buffer to make sure we don't have any garbage in it */
	memset(local_buf, 0, BUF_SIZE);

   /* Lock the list while reading */
	if (mutex_lock_interruptible(&linked_list_lock)) {
		kfree(local_buf);
		return -ERESTARTSYS;
	}

	/* Iterate over the linked list*/
	list_for_each_entry(entry, &task_list, list) {

		if (len >= BUF_SIZE - 1)
			break;

		len += scnprintf(local_buf  + len,
				 	 BUF_SIZE - len,
				 	 "%d\n",
				 	 entry->pid);
		
		/* Prevent overflow */
		if (len >= BUF_SIZE)
			break;
	}
	mutex_unlock(&linked_list_lock);

   /* Copy to userspace */
	ret = simple_read_from_buffer(user_buf,
								  count,
								  ppos,
								  local_buf,
								  len);
	kfree(local_buf);
	return ret;
}

/**
* file_write - copies data from user space, validates PID,
*              and processes the input
* @file: file that is being written to
* @user_buf: buffer in user space containing input
* @count: number of bytes to be written
* @ppos: file offset
*
* Expected input: 
* For REGISTRATION: R PID
* For UNREGISTRATION: U PID
*/
static ssize_t file_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {
	ssize_t bytes_to_copy;                 /* number of bytes to copy */
	ssize_t not_copied;                    /* number of bytes that failed to copy */
	int ret = 0;                           /* return value */
	char local_buf[WRITE_BUF_SIZE];
	char command;
	bool need_start = false;
	bool need_stop = false;

	if (count == 0)
			return -EINVAL;
	
	/* Reject oversized input to prevent buffer overflow */
	if (count >= WRITE_BUF_SIZE) {
		return -EINVAL;
	}

	/* Determine safe number of bytes to copy */
	bytes_to_copy = min(count, (size_t)(WRITE_BUF_SIZE - 1));

	/* Copy data safely from user space to kernel buffer */
	not_copied = copy_from_user(local_buf,
								user_buf,
								bytes_to_copy);
	if (not_copied)
			return -EFAULT;
	
   /* Null terminate the kernel buffer */
	local_buf[bytes_to_copy] = '\0';

   /* Remove trailing new line (in case of using echo) */
	strim(local_buf);

   /* Parse the input */
	command = local_buf[0];
	if (command == '\0')
		return -EINVAL;

    /* Acquire mutex to protect shared linked list */
    if (mutex_lock_interruptible(&linked_list_lock))
        return -ERESTARTSYS;

    switch (command) {
	case 'R':
		ret = handle_register_locked(local_buf, &need_start);
		break;
	case 'U':
		ret = handle_unregister_locked(local_buf, &need_stop);
		break;
    default:
    	ret = -EINVAL;
    	break;
	}

	mutex_unlock(&linked_list_lock);

	if (ret)
		return ret;

	if (need_start)
		start_sampling();

	if (need_stop)
		stop_sampling();

	return count;
}

static struct proc_ops file_ops = {
	.proc_read = file_read,
	.proc_write = file_write,
};

static struct proc_dir_entry *mp3_dir;
static struct proc_dir_entry *status_entry;

// mp1_init - Called when module is loaded
int __init mp3_init(void)
{
	int ret = 0;

	#if DEBUG
	printk(KERN_ALERT "MP3 MODULE LOADING\n");
	#endif

	ret = alloc_profiler_buffer();
	if (ret)
		return ret;

	mp3_wq = alloc_workqueue("mp3_wq", WQ_UNBOUND, 1);
	if (!mp3_wq) {
		ret = -ENOMEM;
		goto err_buf;
	}

	INIT_DELAYED_WORK(&sample_work, sample_work_fn);

	ret = setup_char_device();
	if (ret)
		goto err_wq;

	mp3_dir = proc_mkdir("mp3", NULL);
	if (!mp3_dir) {
		ret = -ENOMEM;
	goto err_chrdev;
	}

	status_entry = proc_create("status", 0666, mp3_dir, &file_ops);
	if (!status_entry) {
		ret = -ENOMEM;
		goto err_proc;
	}

	#if DEBUG
	printk(KERN_ALERT "MP3 MODULE LOADED\n");
	#endif
	return 0;

err_proc:
	proc_remove(mp3_dir);
	mp3_dir = NULL;
err_chrdev:
	cleanup_char_device();
err_wq:
	destroy_workqueue(mp3_wq);
	mp3_wq = NULL;
err_buf:
	free_profiler_buffer();
	return ret;
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
	struct mp3_task_struct *entry, *tmp;

	#if DEBUG
	printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
	#endif

	stop_sampling();

	if (mp3_wq) {
		destroy_workqueue(mp3_wq);
		mp3_wq = NULL;
	}

	mutex_lock(&linked_list_lock);
	list_for_each_entry_safe(entry, tmp, &task_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&linked_list_lock);

	if (status_entry)
		proc_remove(status_entry);
	if (mp3_dir)
		proc_remove(mp3_dir);

	cleanup_char_device();
	free_profiler_buffer();

	#if DEBUG
	printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
	#endif
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
