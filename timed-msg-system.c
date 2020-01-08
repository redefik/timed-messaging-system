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
#include "timed-msg-system.h" 

// root-configurable parameters
static unsigned int max_message_size = MAX_MSG_SIZE_DEFAULT; 
module_param(max_message_size, uint, S_IRUGO | S_IWUSR);
static unsigned int max_storage_size = MAX_STORAGE_SIZE_DEFAULT;
module_param(max_storage_size, uint, S_IRUGO | S_IWUSR);

static int major; // dinamically allocated by the kernel
#ifdef TEST
module_param(major, int, S_IRUGO | S_IWUSR);
#endif
static struct minor_struct minors[MINORS];

// TODO possibly centralize error mgmt
static int dev_open(struct inode *inodep, struct file *filep)
{
	struct session_struct *session_struct;
	
	// Allocate a session_struct object
	session_struct = kmalloc(sizeof(struct session_struct), GFP_KERNEL);
	if (session_struct == NULL) {
		return -ENOMEM;
	}
	// Initalize the object
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
	INIT_LIST_HEAD(&(session_struct->pending_reads));
	init_waitqueue_head(&(session_struct->read_wq));
	// Link the object to struct file
	filep->private_data = (void *)session_struct;
	
	return 0;
}

// TODO possibly modify it after implementing dev_flush()
static int dev_release(struct inode *inodep, struct file *filep)
{
	struct session_struct *session_struct;
	
	// Deallocate session_struct object linked to struct file
	session_struct = (struct session_struct *)filep->private_data;
	destroy_workqueue(session_struct->write_wq);
	kfree(session_struct);	
	
	return 0;
}

static ssize_t dev_read(struct file *filep, char *bufp, size_t len, loff_t *offp)
{
	int minor_idx, ret;
	struct message_struct *msg;
	struct session_struct *session = filep->private_data;
	struct pending_read_struct *pending_read;
	unsigned long read_timeout, to_sleep;
		
	minor_idx = iminor(filep->f_inode); // TODO possibly make it more portable using versioning
	mutex_lock(&(minors[minor_idx].mtx));
	
	// Get the first message in the FIFO queue
	msg = list_first_entry_or_null(&(minors[minor_idx].fifo), 
	                               struct message_struct, list);
	                               
	if (msg != NULL) { // Not empty queue		
		goto deliver_message;
	}
	
	// Empty queue
	
	mutex_unlock(&(minors[minor_idx].mtx));	
	mutex_lock(&(session->mtx));
	read_timeout = session->read_timeout;
	
	if (!read_timeout) { // Non-blocking read
		mutex_unlock(&(session->mtx));
		return -EAGAIN;
	}
	
	// Blocking read
	
	to_sleep = read_timeout;
	// Allocate a pending_read_struct object
	pending_read = kmalloc(sizeof(struct pending_read_struct), GFP_KERNEL);
	if (pending_read == NULL) {
		return -ENOMEM;
	}
	// Initialize a pending_read_struct
	pending_read->msg_available = 0;
	INIT_LIST_HEAD(&(pending_read->list));
	// Insert the object in the queue of the pending reads
	list_add_tail(&(pending_read->list), &(session->pending_reads));
	mutex_unlock(&(session->mtx));
	
	// TODO to be modified after implementing flush()
	// Go to sleep waiting for available messages
	while (to_sleep) {
		ret = wait_event_interruptible_timeout(session->read_wq, 
		                                       pending_read->msg_available,
		                                       to_sleep);
		if (ret == -ERESTARTSYS) { // sleep interrupted by a signal
			goto remove_pending_read;
		}
		if (ret == 0) { // empty list after timer expiration
			ret = -ETIME;
			goto remove_pending_read;
		}
		// Check if the list is actually not empty
		mutex_lock(&(minors[minor_idx].mtx));
		msg = list_first_entry_or_null(&(minors[minor_idx].fifo), 
	                                   struct message_struct, list);		
		if (msg == NULL) { // the list is actually empty so return to sleep
			mutex_unlock(&(minors[minor_idx].mtx));
			pending_read->msg_available = 0;
			mutex_lock(&(session->mtx));
			list_add_tail(&(pending_read->list), &(session->pending_reads));
			mutex_unlock(&(session->mtx));
			to_sleep = ret;
		} else { // a message is actually available
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
	mutex_lock(&(session->mtx));
	list_del(&(pending_read->list));
	mutex_unlock(&(session->mtx));
	kfree(pending_read);
	return ret;	
}

// TODO Consider the possibility to make it an inline function to reduce overhead
static int post_message(int minor_idx, char *kbuf, size_t len)
{
	struct message_struct *msg;
	
	mutex_lock(&(minors[minor_idx].mtx));
	if (minors[minor_idx].current_size + len > max_storage_size) {
		mutex_unlock(&(minors[minor_idx].mtx));
		kfree(kbuf);
		return -EAGAIN;
	}
	msg = kmalloc(sizeof(struct message_struct), GFP_KERNEL);
	if (msg == NULL) {
		mutex_unlock(&(minors[minor_idx].mtx));
		kfree(kbuf);
		return -ENOMEM;
	}
	msg->size = len;
	msg->buf = kbuf;
	INIT_LIST_HEAD(&(msg->list));
	list_add_tail(&(msg->list),&(minors[minor_idx].fifo));
	minors[minor_idx].current_size += len;
	mutex_unlock(&(minors[minor_idx].mtx));
	return len;
}

// TODO Consider make it inlined
static void awake_pending_reader(struct session_struct *session)
{
	struct pending_read_struct *pending_read;
	
	mutex_lock(&(session->mtx));
	pending_read = list_first_entry_or_null(&(session->pending_reads), 
	                                        struct pending_read_struct, list);
	if (pending_read) {
		list_del(&(pending_read->list));
		pending_read->msg_available = 1;
		wake_up_interruptible(&(session->read_wq));
	}
	mutex_unlock(&(session->mtx));
}

static void deferred_write(struct work_struct *work_struct)
{
	int ret;
	struct delayed_work *delayed_work;
	struct pending_write_struct *pending_write;
	
	// work_struct is embedded in a struct delayed_work (work field)
	// delayed_work is embedded in a struct pending_write_struct
	// so twice invokation of containener_of is needed	
	delayed_work = container_of(work_struct, struct delayed_work, work);
	pending_write = container_of(delayed_work, struct pending_write_struct, 
	                                           delayed_work);
	// Remove the deferred work from the list of pending writes
	mutex_lock(&(pending_write->session->mtx));
	list_del(&(pending_write->list));
	mutex_unlock(&(pending_write->session->mtx));
	
	ret = post_message(pending_write->minor, 
	                   pending_write->kbuf, pending_write->len);
	if (ret >= 0) { // message post succeeded
		awake_pending_reader(pending_write->session);
	}	
	
	kfree(pending_write);		
	return;
}

/**
 * dev_write - Write a message into the device file
 * @filep: pointer to struct file
 * @bufp: pointer to user buffer containing the message
 * @len: message size
 * @offp: unused
 *
 * Returns:
 * - the length of the written message, if the non-blocking mode is set and the
 * - operation succeeds.
 * - %EMSGSIZE if the message is too long (len > max_message_size)
 * - %ENOMEM if allocation of used kernel buffers fails
 * - %EFAULT if @bufp points to an illegal memory area
 * - %EAGAIN if the device file is temporary full
 * - %0 if a write timeout exists. In that case, the actual write is delayed.
 *
 * NOTE that when the write is delayed, it may fail in the absence of free
 * space in the device file. 
 */
static ssize_t dev_write(struct file *filep, const char *bufp, size_t len, loff_t *offp)
{
	char *kbuf;
	int minor_idx, ret;
	struct pending_write_struct *pending_write;
	struct session_struct *session = (struct session_struct *)filep->private_data;

	if (len > max_message_size) {
		return -EMSGSIZE;
	}

	// Allocate a kernel buffer
	kbuf = kmalloc(len, GFP_KERNEL);
	if (kbuf == NULL) {
		return -ENOMEM;
	}	
	
	// Copy the message in the kernel buffer
	if (copy_from_user(kbuf, bufp, len)) {
		kfree(kbuf);
		return -EFAULT;
	}
	
	minor_idx = iminor(filep->f_inode); // TODO possibly, make it more portable using versioning

	mutex_lock(&(session->mtx));
	if (session->write_timeout) { // a write timeout exists
		// Allocate a pending_write_struct object
		pending_write = kmalloc(sizeof(struct pending_write_struct), GFP_KERNEL);
		if (pending_write == NULL) {
			kfree(kbuf);
			mutex_unlock(&(session->mtx));
			return -ENOMEM;
		}
		// Initialize the object
		pending_write->minor = minor_idx;
		pending_write->session = session;
		pending_write->kbuf = kbuf;
		pending_write->len = len;
		INIT_LIST_HEAD(&(pending_write->list));
		INIT_DELAYED_WORK(&(pending_write->delayed_work), deferred_write);
		// Add the object to the list of pending writes linked to the session
		list_add_tail(&(pending_write->list),&(session->pending_writes));
		// Defer the write using the write workqueue
		queue_delayed_work(session->write_wq, &(pending_write->delayed_work), 
		                   session->write_timeout);
		mutex_unlock(&(session->mtx));
		return 0; // no byte actually written                                                 
	}
	
	mutex_unlock(&(session->mtx));
	
	// Immediate write
	ret = post_message(minor_idx, kbuf, len);
	if (ret >= 0) { // message post succeeded
		awake_pending_reader(session);
	}
	
	return ret;
}

// TODO possibly provide a more fine-grained timeout mechanism
/**
* dev_ioctl - modify the operating mode of read() and write()
* @filep: pointer to struct file
* @cmd: one of the macro defined in timed-msg-system.h (%SET_SEND_TIMEOUT,
* %SET_RECV_TIMEOUT, %REVOKE_DELAYED_MESSAGES)
* @arg: read/write timeout (optional)
*
* Returns:
* - 0 if the operation succeeds
* - %ENOTTY if the provided command is not valid
*
* If %SET_SEND_TIMEOUT is provided, the write timeout of the current session
* is set to the value @arg.
* If %SET_RECV_TIMEOUT is provided, the read timeout of the current session
* is set to the value @arg.
* If %REVOKE_DELAYED_MESSAGES is provided, the pending writes are undone.
*
* NOTE @arg is interpreted as milliseconds and the granularity of the actual
* timeout is the one of jiffies. Therefore, according to the value of HZ,
* the timeout may be set to 0 if too short.
*/
static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct session_struct *session = (struct session_struct *)filep->private_data;
	struct list_head *ptr;
	struct list_head *tmp;
	struct pending_write_struct *pending_write;

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
			// Scan pending writes
			list_for_each_safe(ptr, tmp, &(session->pending_writes)) {
				pending_write = list_entry(ptr, struct pending_write_struct, list);
				// Cancel delayed write
				// NOTE that the pending write may be actually already in execution
				// thus we have to check return value
				if (cancel_delayed_work(&(pending_write->delayed_work))) {
					list_del(&(pending_write->list));
					kfree(pending_write->kbuf);
					kfree(pending_write);
				}
			}
			mutex_unlock(&(session->mtx));
			break;
		default:
			printk(KERN_INFO "%s: ioctl() command not valid\n", MODNAME);
			return -ENOTTY;
	}
	return 0;
}

static int dev_flush(struct file *filep, fl_owner_t id)
{
	// TODO
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
	
	//Initialization of minor_struct array
	for (i = 0; i < MINORS; i++) {
		minors[i].current_size = 0;
		mutex_init(&(minors[i].mtx));
		INIT_LIST_HEAD(&(minors[i].fifo));
	}
	
	// driver registration
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
		// flush FIFO queue
		list_for_each_safe(ptr, tmp, &(minors[i].fifo)) {
			msg = list_entry(ptr, struct message_struct, list);
			list_del(&(msg->list));
			kfree(msg->buf);
			kfree(msg);			
		}
		mutex_unlock(&(minors[i].mtx));
	}
	
	// driver unregistration
	unregister_chrdev(major, DEVICE_NAME);
	printk(KERN_INFO "%s: Driver correctly uninstalled\n", MODNAME);
	return;
}

module_init(install_driver);
module_exit(uninstall_driver);

MODULE_AUTHOR("Federico Viglietta");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("The module provides a device file that allows exchanging messages across threads");//TODO refine DESCRIPTION
