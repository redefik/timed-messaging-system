#include <linux/ioctl.h>

#define MAGIC_BASE 'k' // NOTE it should be unique across the system
#define SET_SEND_TIMEOUT _IO(MAGIC_BASE, 0)
#define SET_RECV_TIMEOUT _IO(MAGIC_BASE, 1)
#define REVOKE_DELAYED_MESSAGES _IO(MAGIC_BASE, 2)

#ifdef __KERNEL__
#define TEST // comment in "production"

#define MODNAME "TIMED-MSG-SYSTEM"
#define DEVICE_NAME "timed-msg-device"
#define MINORS 3 // supported minor numbers
#define MAX_MSG_SIZE_DEFAULT 4096 // bytes
#define MAX_STORAGE_SIZE_DEFAULT 65536 // bytes
#define WRITE_WORK_QUEUE "wq-timed-msg-system"// Represents a node of the FIFO queue containing messages
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
	struct list_head sessions; // open I/O sessions
	struct list_head pending_reads; // points to list of pending reads
	wait_queue_head_t read_wq; // used to implement blocking reads
};

// Represents a deferred write
struct pending_write_struct {
	int minor; // instance of the device file involved in the write
	struct session_struct *session; // session involved in the write
	char *kbuf; // temporary buffer containing the message to write
	unsigned int len; // message length
	struct delayed_work delayed_work;
	struct list_head list; // used to link the node in a list
};

// Used by a blocking read to sleep waiting for available messages
struct pending_read_struct {
	int msg_available;
	int flushing;
	struct list_head list; // used to link the node in a list	
};

// Extra information about an I/O session
struct session_struct {
	struct mutex mtx;
	struct workqueue_struct *write_wq; // used to defer writes
	unsigned long write_timeout; // 0 means immediate storing
	unsigned long read_timeout; // 0 means non-blocking reads in the absence of messages
	struct list_head pending_writes; // points to list of deferred writes
	struct list_head list; // used to concatenate nodes in a list of sessions related to a an instance of the device file
};

// Supported file operations
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);
static int dev_flush(struct file *, fl_owner_t id);

#endif
