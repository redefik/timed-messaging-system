/*
* @brief This module provides a device file that allows exchanging messages
*        across threads
* @author Federico Viglietta
* @date January 10, 2019
*/


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/param.h>
#include <linux/wait.h>
#include <linux/version.h>
#include "timed-msg-system.h" 

/* Parameters reconfigurable by root */
static unsigned int max_message_size = MAX_MSG_SIZE_DEFAULT; 
module_param(max_message_size, uint, S_IRUGO | S_IWUSR);
static unsigned int max_storage_size = MAX_STORAGE_SIZE_DEFAULT;
module_param(max_storage_size, uint, S_IRUGO | S_IWUSR);

static int major;
static struct minor_struct minors[MINORS];

/* Portable minor number retrieval */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define fminor(filep) iminor(filep->f_inode)
#else
#define fminor(filep) iminor(filep->f_entry->d_inode)
#endif

static int dev_open(struct inode *inodep, struct file *filep)
{
	struct session_struct *session_struct;
	int minor_idx;
	
	/* Allocate a session_struct */
	session_struct = kmalloc(sizeof(struct session_struct), GFP_KERNEL);
	if (session_struct == NULL) {
		return -ENOMEM;
	}
	/* Initialize the session_struct */
	mutex_init(&(session_struct->mtx));
	session_struct->write_wq = alloc_workqueue(WRITE_WORK_QUEUE, 
	                                           WQ_MEM_RECLAIM, 0);
	if (session_struct->write_wq == NULL) {
		kfree(session_struct);
		return -ENOMEM;
	}
	session_struct->write_timeout = 0;
	session_struct->read_timeout = 0;
	INIT_LIST_HEAD(&(session_struct->pending_writes));
	INIT_LIST_HEAD(&(session_struct->list));
	/* Link the session_struct to the struct file */
	filep->private_data = (void *)session_struct;
	/* Link the session_struct to the minor_struct */
	minor_idx = iminor(inodep);
	mutex_lock(&(minors[minor_idx].mtx));
	list_add_tail(&(session_struct->list), &(minors[minor_idx].sessions));
	mutex_unlock(&(minors[minor_idx].mtx));	
	return 0;
}

static ssize_t dev_read(struct file *filep, char *bufp, size_t len, loff_t *offp)
{
	int minor_idx, ret;
	struct message_struct *msg;
	struct session_struct *session;
	struct pending_read_struct *pending_read;
	unsigned long read_timeout, to_sleep;
		
	session = (struct session_struct *)filep->private_data;
	minor_idx = fminor(filep);

	mutex_lock(&(minors[minor_idx].mtx));
	
	/* Retrieve the first message stored in the device file */
	msg = list_first_entry_or_null(&(minors[minor_idx].fifo), 
	                               struct message_struct, list);
	                               
	if (msg != NULL) { /* Not empty queue */		
		goto deliver_message;
	}
	
	/* Empty queue*/
	mutex_unlock(&(minors[minor_idx].mtx));	
	mutex_lock(&(session->mtx));
	read_timeout = session->read_timeout;
	mutex_unlock(&(session->mtx));
	if (!read_timeout) { /* Non-blocking read */
		return -ENOMSG;
	}
	
	/* Blocking read */
	to_sleep = read_timeout;
	/* Allocate a pending_read_struct */
	pending_read = kmalloc(sizeof(struct pending_read_struct), GFP_KERNEL);
	if (pending_read == NULL) {
		return -ENOMEM;
	}
	/* Initialize the pending_read_struct */
	pending_read->msg_available = 0;
	pending_read->flushing = 0;
	INIT_LIST_HEAD(&(pending_read->list));
	mutex_lock(&(minors[minor_idx].mtx));
	/* Enqueue the pending read to the others */
	list_add_tail(&(pending_read->list), &(minors[minor_idx].pending_reads));
	mutex_unlock(&(minors[minor_idx].mtx));
	
	/* Go to sleep waiting for available messages */
	while (to_sleep) {
		ret = wait_event_interruptible_timeout(minors[minor_idx].read_wq, 
		                                       pending_read->msg_available || pending_read->flushing,
		                                       to_sleep);
		if (ret == -ERESTARTSYS) { /* signal delivered during sleep */
			if (pending_read->msg_available || pending_read->flushing) {
				goto free_pending_read;
			} else {
				goto remove_pending_read;
			}
		}
		if (ret == 0) { /* empty list after timer expiration */
			ret = -ETIME;
			goto remove_pending_read;
		}
		if (pending_read->flushing) { /* dev_flush() invoked */
			ret = -ECANCELED;
			goto free_pending_read;
		}
		/* A message should be available */
		
		/* Check if the list is actually not empty */
		mutex_lock(&(minors[minor_idx].mtx));
		msg = list_first_entry_or_null(&(minors[minor_idx].fifo), 
	                                   struct message_struct, list);		
		if (msg == NULL) { /* list actually empty, return to sleep */
			pending_read->msg_available = 0;
			list_add_tail(&(pending_read->list), 
				      &(minors[minor_idx].pending_reads));
			mutex_unlock(&(minors[minor_idx].mtx));
			to_sleep = ret;
		} else { /* message actually available */
			kfree(pending_read);
			goto deliver_message;
		}
	}

deliver_message:
	if (len > msg->size) {
		len = msg->size;
	}
	if (copy_to_user(bufp, msg->buf, len)) {
		mutex_unlock(&(minors[minor_idx].mtx));
		return -EFAULT;
	}
	list_del(&(msg->list));
	minors[minor_idx].current_size -= msg->size;
	mutex_unlock(&(minors[minor_idx].mtx));
	kfree(msg->buf);
	kfree(msg);
	return len;
remove_pending_read:
	mutex_lock(&(minors[minor_idx].mtx));
	list_del(&(pending_read->list));
	mutex_unlock(&(minors[minor_idx].mtx));
free_pending_read:
	kfree(pending_read);
	return ret;	
}

/**
* __post_message - Actually write a message into a device file
* 
* @minor: pointer to %minor_struct representing the target device file
* @kbuf: pointer to the kernel buffer containing the message to be posted
* @len: size of the message
*
* Returns the number of written bytes on success. Otherwise, it returns
* %-EAGAIN if the device file has no free space or %-ENOMEM if it fails in
* allocating the %message_struct to post
* */
static int __post_message(struct minor_struct *minor, char *kbuf, size_t len)
{
	struct message_struct *msg;
	
	if (minor->current_size + len > max_storage_size) {
		kfree(kbuf);
		return -ENOSPC;
	}
	msg = kmalloc(sizeof(struct message_struct), GFP_KERNEL);
	if (msg == NULL) {
		kfree(kbuf);
		return -ENOMEM;
	}
	msg->size = len;
	msg->buf = kbuf;
	INIT_LIST_HEAD(&(msg->list));
	list_add_tail(&(msg->list),&(minor->fifo));
	minor->current_size += len;
	
	return len;
}

/**
* __awake_pending_reader - Awakes a reader waiting for messages
* 
* @minor: pointer to %minor_struct representing the device file
*
*/
static void __awake_pending_reader(struct minor_struct *minor)
{
	struct pending_read_struct *pending_read;
	
	pending_read = list_first_entry_or_null(&(minor->pending_reads), 
	                                        struct pending_read_struct, 
	                                        list);
	if (pending_read) {
		list_del(&(pending_read->list));
		pending_read->msg_available = 1;
		wake_up_interruptible(&(minor->read_wq));
	}
	return;
}

/**
* __deferred_write - Write a message in a device file after a delay
* 
* @work_struct: pointer to %struct work_struct
*
* NOTE the %struct work_struct is embedded inside a %struct delayed_work.
* This is embedded too inside a %struct pending_write_struct
*/
static void __deferred_write(struct work_struct *work_struct)
{
	int ret;
	struct delayed_work *delayed_work;
	struct pending_write_struct *pending_write;
		
	delayed_work = container_of(work_struct, struct delayed_work, work);
	pending_write = container_of(delayed_work, struct pending_write_struct, 
	                                           delayed_work);
	/* Dequeue from the list of pending writes */
	mutex_lock(&(pending_write->session->mtx));
	list_del(&(pending_write->list));
	mutex_unlock(&(pending_write->session->mtx));
	
	mutex_lock(&(minors[pending_write->minor].mtx));
	ret = __post_message(&minors[pending_write->minor], 
	                   pending_write->kbuf, pending_write->len);
	if (ret >= 0) { /* message post succeeded */
		__awake_pending_reader(&(minors[pending_write->minor]));
	}
	mutex_unlock(&(minors[pending_write->minor].mtx));	
	
	kfree(pending_write);		
	return;
}

static ssize_t dev_write(struct file *filep, const char *bufp, size_t len, loff_t *offp)
{
	char *kbuf;
	int minor_idx, ret;
	struct pending_write_struct *pending_write;
	struct session_struct *session; 
	
	session = (struct session_struct *)filep->private_data;

	if (len > max_message_size) {
		return -EMSGSIZE;
	}

	/* Allocate a kernel buffer */
	kbuf = kmalloc(len, GFP_KERNEL);
	if (kbuf == NULL) {
		return -ENOMEM;
	}	
	
	/* Copy the message in the kernel buffer */
	if (copy_from_user(kbuf, bufp, len)) {
		kfree(kbuf);
		return -EFAULT;
	}
	
	minor_idx = fminor(filep);

	mutex_lock(&(session->mtx));
	if (session->write_timeout) { /* a write timeout exists */		
		/* Allocate a pending_write_struct */
		pending_write = kmalloc(sizeof(struct pending_write_struct), 
		                        GFP_KERNEL);
		if (pending_write == NULL) {
			kfree(kbuf);
			mutex_unlock(&(session->mtx));
			return -ENOMEM;
		}
		/* Initialize the pending_write_struct */
		pending_write->minor = minor_idx;
		pending_write->session = session;
		pending_write->kbuf = kbuf;
		pending_write->len = len;
		INIT_LIST_HEAD(&(pending_write->list));
		INIT_DELAYED_WORK(&(pending_write->delayed_work), __deferred_write);
		/* Enqueue the pending write to the list of the others */
		list_add_tail(&(pending_write->list),&(session->pending_writes));
		mutex_unlock(&(session->mtx));
		queue_delayed_work(session->write_wq, 
		                   &(pending_write->delayed_work), 
		                   session->write_timeout);
		return 0; /* no byte actually written */                                                 
	}
	
	mutex_unlock(&(session->mtx));
	
	/* Immediate storing */
	mutex_lock(&(minors[minor_idx].mtx));
	ret = __post_message(&minors[minor_idx], kbuf, len);
	if (ret >= 0) { /* message post succeeded */
		__awake_pending_reader(&(minors[minor_idx]));
	}
	mutex_unlock(&(minors[minor_idx].mtx));
	
	return ret;
}

/**
* __revoke_delayed_messages - Cancel delayed write of an I/O session
*
* @session: pointer to %struct session_struct representing the I/O session
*
*/
static void __revoke_delayed_messages(struct session_struct *session)
{
	struct list_head *ptr;
	struct list_head *tmp;
	struct pending_write_struct *pending_write;

	list_for_each_safe(ptr, tmp, &(session->pending_writes)) {
		pending_write = list_entry(ptr, struct pending_write_struct, 
		                           list);

		/* NOTE that the pending write may be actually already in execution
		   thus we have to check return value */
		if (cancel_delayed_work(&(pending_write->delayed_work))) {
			list_del(&(pending_write->list));
			kfree(pending_write->kbuf);
			kfree(pending_write);
		}
	}
}

// TODO possibly provide a more fine-grained timeout mechanism
static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct session_struct *session;
	
	session = (struct session_struct *)filep->private_data;

	switch (cmd) {
	case SET_SEND_TIMEOUT:
		mutex_lock(&(session->mtx));
		session->write_timeout = (arg * HZ)/1000;
		mutex_unlock(&(session->mtx));
		break;
	case SET_RECV_TIMEOUT:
		mutex_lock(&(session->mtx));
		session->read_timeout = (arg * HZ)/1000;
		mutex_unlock(&(session->mtx));
		break;
	case REVOKE_DELAYED_MESSAGES:
		mutex_lock(&(session->mtx));
		__revoke_delayed_messages(session);
		mutex_unlock(&(session->mtx));
		break;
	default:
		printk(KERN_INFO "%s: ioctl() command not valid\n", MODNAME);
		return -ENOTTY;
	}
	return 0;
}

/**
* __unblock_reads - Unblock readers waiting for messages
*
* @minor: pointer to %struct minor_struct representing the target device file
*
*/
static void __unblock_reads(struct minor_struct *minor)
{
	struct list_head *ptr;
	struct list_head *tmp;
	struct pending_read_struct *pending_read;
	
	list_for_each_safe(ptr, tmp, &(minor->pending_reads)) {
		pending_read = list_entry(ptr, struct pending_read_struct,
					  list);
		pending_read->flushing = 1;
		list_del(&(pending_read->list));
		wake_up_interruptible(&(minor->read_wq));			  
	}
}

static int dev_flush(struct file *filep, fl_owner_t id)
{
	int minor_idx;
	struct list_head *ptr;
	struct session_struct *session;
	
	minor_idx = fminor(filep);
	mutex_lock(&(minors[minor_idx].mtx));
	/* Revoke delayed writes */
	list_for_each(ptr, &(minors[minor_idx].sessions)) {
		session = list_entry(ptr, struct session_struct, list);
		mutex_lock(&(session->mtx));
		__revoke_delayed_messages(session);
		mutex_unlock(&(session->mtx));
	}
	/* Readers waiting for messages are unblocked */
	__unblock_reads(&(minors[minor_idx]));
	mutex_unlock(&(minors[minor_idx].mtx));
	
	return 0;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
	struct session_struct *session_struct;
	int minor_idx;
	
	session_struct = (struct session_struct *)filep->private_data;
	/* Wait for delayed write in execution to complete */
	flush_workqueue(session_struct->write_wq);
	destroy_workqueue(session_struct->write_wq);
	/* Unlink session_struct from minor_struct */
	minor_idx = iminor(inodep);
	mutex_lock(&(minors[minor_idx].mtx));
	list_del(&(session_struct->list));
	mutex_unlock(&(minors[minor_idx].mtx));
	
	kfree(session_struct);
	
	return 0;
}

static struct file_operations fops = 
{
	.owner = THIS_MODULE,
	.open = dev_open,
	.release = dev_release,
	.read = dev_read,
	.write = dev_write,
	.unlocked_ioctl = dev_ioctl,
	.flush = dev_flush,
};

static int __init install_driver(void)
{
	int i;
	
	/* Initialization of minor_struct array */
	for (i = 0; i < MINORS; i++) {
		minors[i].current_size = 0;
		mutex_init(&(minors[i].mtx));
		INIT_LIST_HEAD(&(minors[i].pending_reads));
		init_waitqueue_head(&(minors[i].read_wq));
		INIT_LIST_HEAD(&(minors[i].fifo));
		INIT_LIST_HEAD(&(minors[i].sessions));
	}
	
	/* Driver registration */
	major = __register_chrdev(0, 0, MINORS, DEVICE_NAME, &fops);
	if (major < 0) {
		printk(KERN_INFO "%s: Driver installation failed\n", MODNAME);
		return major;
	}
	printk(KERN_INFO "%s: Driver correctly installed, MAJOR = %d\n", MODNAME, 
	       major);
	return 0;
	
}

static void __exit uninstall_driver(void)
{
	int i;
	struct list_head *ptr;
	struct list_head *tmp;
	struct message_struct *msg;
	
	for (i = 0; i < MINORS; i++) {
		mutex_lock(&(minors[i].mtx));
		/* Flush content of the device files */
		list_for_each_safe(ptr, tmp, &(minors[i].fifo)) {
			msg = list_entry(ptr, struct message_struct, list);
			list_del(&(msg->list));
			kfree(msg->buf);
			kfree(msg);			
		}
		mutex_unlock(&(minors[i].mtx));
	}
	
	/* Driver unregistration */
	unregister_chrdev(major, DEVICE_NAME);
	printk(KERN_INFO "%s: Driver correctly uninstalled\n", MODNAME);
	return;
}

module_init(install_driver);
module_exit(uninstall_driver);

MODULE_AUTHOR("Federico Viglietta");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This module provides a device file that allows exchanging messages across threads");
