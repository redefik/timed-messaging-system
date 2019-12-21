#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/errno.h>

#define MODNAME "TIMED-MSG-SYSTEM"
#define DEVICE_NAME "timed-msg-device"
#define MINORS 3 // supported minor numbers

static unsigned int max_message_size; 
module_param(max_message_size, uint, S_IRUGO | S_IWUSR);
static unsigned int max_storage_size; 
module_param(max_storage_size, uint, S_IRUGO | S_IWUSR);

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
	// TODO
	return 0;
}

static ssize_t dev_write(struct file *filep, const char *bufp, size_t len, loff_t *offp)
{
	// TODO
	return 0;
}

static long dev_ioctl(struct file *filep, unsigned int c, unsigned long v)
{
	// TODO
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

static int major; // dinamically allocated by the kernel

static int __init install_driver(void)
{
	//TODO Initialization of minor_structs
	
	// driver registration
	major = __register_chrdev(0, 0, MINORS, DEVICE_NAME, &fops);
	if (major < 0) {
		printk(KERN_INFO "%s: Driver installation failed\n", MODNAME);
		return major;
	}
	printk(KERN_INFO "%s: Driver correctly installed, MAJOR = %d\n", MODNAME, major);
	return 0;
	
}

static void __exit uninstall_driver(void)
{
	// TODO Deallocate minor_structs
	
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
