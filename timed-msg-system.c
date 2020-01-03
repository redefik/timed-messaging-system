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
#include "timed-msg-system.h" // TODO possibly move to this file some of the following definitions

#define TEST // uncomment in "production"

#define MODNAME "TIMED-MSG-SYSTEM"
#define DEVICE_NAME "timed-msg-device"
#define MINORS 3 // supported minor numbers
#define MAX_MSG_SIZE_DEFAULT 4096 // bytes
#define MAX_STORAGE_SIZE_DEFAULT 65536 // bytes

// root-configurable parameters
static unsigned int max_message_size = MAX_MSG_SIZE_DEFAULT; 
module_param(max_message_size, uint, S_IRUGO | S_IWUSR);
static unsigned int max_storage_size = MAX_STORAGE_SIZE_DEFAULT;
module_param(max_storage_size, uint, S_IRUGO | S_IWUSR);

// Represents a node of the FIFO queue containing messages
struct message_struct {
	unsigned int size; // must be <= max_message_size
	char *buf; // points to the buffer containing the message
	struct list_head list; // used to concatenate nodes of the FIFO queue
};

// Represents an istance of the device file
struct minor_struct {
	unsigned int current_size; // must be <= max_storage_size
	struct mutex mtx;
	struct list_head fifo; // points to the FIFO queue of message_struct
};

static int major; // dinamically allocated by the kernel
#ifdef TEST
module_param(major, int, S_IRUGO | S_IWUSR);
#endif
static struct minor_struct minors[MINORS];

// Supported file operations
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);
static int dev_flush(struct file *, fl_owner_t id);

static int dev_open(struct inode *inodep, struct file *filep)
{
	// TODO
	return 0;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
	// TODO
	return 0;
}

static ssize_t dev_read(struct file *filep, char *bufp, size_t len, loff_t *offp)
{
	int minor_idx;
	struct message_struct *msg;
	
	minor_idx = iminor(filep->f_inode); // TODO possibly make it more portable using versioning
	mutex_lock(&(minors[minor_idx].mtx));
	
	// Get the first message in the FIFO queue
	msg = list_first_entry_or_null(&(minors[minor_idx].fifo), 
	                               struct message_struct, list);
	if (msg == NULL) { // Empty queue
		mutex_unlock(&(minors[minor_idx].mtx));
		return -EAGAIN;
	}
	
	// Try to deliver the message to the user
	if (len > msg->size) {
		len = msg->size;
	}
	if (copy_to_user(bufp, msg->buf, len)) {
		mutex_unlock(&(minors[minor_idx].mtx));
		return -EFAULT;
	}
	
	// Dequeue the message only now, to avoid loss of messages
	list_del(&(msg->list));
	// Update the size of the device file
	minors[minor_idx].current_size -= msg->size;
	
	mutex_unlock(&(minors[minor_idx].mtx));
	kfree(msg->buf);
	kfree(msg);
	
	return len;
	
}

// TODO possibly centralize error management
static ssize_t dev_write(struct file *filep, const char *bufp, size_t len, loff_t *offp)
{
	char *kbuf;
	int minor_idx;
	struct message_struct *msg;

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
	mutex_lock(&(minors[minor_idx].mtx));
	
	if (minors[minor_idx].current_size + len > max_storage_size) {
		mutex_unlock(&(minors[minor_idx].mtx));
		kfree(kbuf);
		return -EAGAIN; // device file is temporary full
	}
	
	// Allocate a message_struct
	msg = kmalloc(sizeof(struct message_struct), GFP_KERNEL);
	if (msg == NULL) {
		mutex_unlock(&(minors[minor_idx].mtx));
		kfree(kbuf);
		return -ENOMEM;
	}
	// Initialize the message_struct
	msg->size = len;
	msg->buf = kbuf;
	INIT_LIST_HEAD(&(msg->list));
	// Enqueue the message_struct
	list_add_tail(&(msg->list), &(minors[minor_idx].fifo));
	
	// Update device file size
	minors[minor_idx].current_size += len;
	
	mutex_unlock(&(minors[minor_idx].mtx));
	
	return len;
}

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	// TODO 
	switch (cmd) {
		case SET_SEND_TIMEOUT:
			printk(KERN_INFO "%s: SET_SEND_TIMEOUT with arg:%lu\n", MODNAME, arg);
			break;
		case SET_RECV_TIMEOUT:
			printk(KERN_INFO "%s: SET_RECV_TIMEOUT with arg:%lu\n", MODNAME, arg);
			break;
		case REVOKE_DELAYED_MESSAGES:
			printk(KERN_INFO "%s: REVOKE_DELAYED_MESSAGES\n", MODNAME);
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
