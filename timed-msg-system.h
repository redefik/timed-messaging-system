#include <linux/ioctl.h>

/******************************ioctl() commands*********************************/

#define MAGIC_BASE 'k' /* NOTE This should be unique */
#define SET_SEND_TIMEOUT _IO(MAGIC_BASE, 0)
#define SET_RECV_TIMEOUT _IO(MAGIC_BASE, 1)
#define REVOKE_DELAYED_MESSAGES _IO(MAGIC_BASE, 2)

/**********************************kernel part**********************************/

#ifdef __KERNEL__

#define MODNAME "TIMED-MSG-SYSTEM"
#define DEVICE_NAME "timed-msg-device"
#define MINORS 3
#define MAX_MSG_SIZE_DEFAULT 4096      /* bytes */
#define MAX_STORAGE_SIZE_DEFAULT 65536 /* bytes */
#define WRITE_WORK_QUEUE "wq-timed-msg-system"

/******************************Data Structures**********************************/

/**
* message_struct - Message stored in an instance of the device file
*/
struct message_struct {
	unsigned int size;
	char *buf;
	struct list_head list;
};

/**
* minor_struct - Instance of a device file
*/
struct minor_struct {
	unsigned int current_size;
	struct mutex mtx;
	struct list_head fifo;          /* Messages stored in the device file */
	struct list_head sessions;
	struct list_head pending_reads; 
	wait_queue_head_t read_wq;      /* Used from blocking readers to wait for messages */
};

/**
* pending_write_struct - Delayed write information
*/
struct pending_write_struct {
	int minor;
	struct session_struct *session;
	char *kbuf;                     /* Points to the message to post */
	unsigned int len;               /* Size of the message to post */
	struct delayed_work delayed_work;
	struct list_head list;
};

/**
* pending_read_struct - A read waiting for available messages
*/
struct pending_read_struct {
	int msg_available; /* Set from a writer when a new message is available */
	int flushing;      /* Set when someone calls dev_flush() */
	struct list_head list;	
};

/**
* session_struct - I/O session auxiliary information
*/
struct session_struct {
	struct mutex mtx;
	struct workqueue_struct *write_wq; /* Used to defer writes*/
	unsigned long write_timeout;       /* 0 means immediate storing */
	unsigned long read_timeout;        /* 0 means non-blocking reads */
	struct list_head pending_writes;
	struct list_head list;
};

/**************************Supported File Operations****************************/

/**
* dev_open - Initialize an I/O session to the device file
* 
* @inodep: pointer to %struct inode representing the device file
* @filep: pointer to %struct file associated to the I/O session
*
* Returns %0 on success, %-ENOMEM if it fails in allocating auxiliary data
* structures
* 
* This function initializes a %session_struct object and links it to struct file
* using the available field %private_data
*/
static int dev_open(struct inode *, struct file *);

/**
* dev_release - release an I/0 session to the device file
* 
* @inodep: pointer to %struct inode representing the device file
* @filep: pointer to %struct file representing the I/O session to be released
* 
* Returns 0
*
* This function deallocates the %session_struct associated to the given I/O
* session (stored in %private_data field)
* NOTE It is not invoked every time a process calls close. Whenever a
* %file structure is shared (e.g. after a fork), it won't be invoked
* until all copies are closed
* NOTE This function waits for running deferred write (those not canceled by
* %dev_flush()) to complete
*/
static int dev_release(struct inode *, struct file *);

/**
* dev_read - Read a message from the device file
*
* @filep: pointer to %struct file representing I/O session
* @bufp: user buffer used to deliver the message
* @len: buffer size
* @offp: unused
*
* Returns the number of read bytes on success. Otherwise, it returns:
* - %-ENOMSG if no message is available and the operating mode of
*   the I/O session is non-blocking (read timeout equal to 0)
* - %-ENOMEM if it fails in allocating a %pending_read_struct
* - %-ERESTARTSYS if the blocking read is interrupted by a signal
* - %-ECANCELED if during a blocking read someone reset the state of the 
*   device file through dev_flush()
* - %-ETIME if timeout expired
* - %-EFAULT if the provided buffer is illegal
*
* NOTE The message receipt fully invalidates the content of the message to
*      be delivered, even if the read() operation requests less bytes than
*      the current size of the message.
*/
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);

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
 * - %ENOSPC if the device file is temporary full
 * - %0 if a write timeout exists. In that case, the actual write is delayed.
 *
 * NOTE that when the write is delayed, it may fail in the absence of free
 * space in the device file. 
 */
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);

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
static long dev_ioctl(struct file *, unsigned int, unsigned long);

/**
* dev_flush - Reset the state of the device file
* 
* @filep: pointer to %struct file representing the I/O session linked to the
*         caller
* @id:unused
*
* Returns 0
*
* NOTE This function causes all threads waiting for messages (along any session)
* to be unblocked and all the delayed messages not yet delivered (along any session)
* to be revoked.
* NOTE This function is called every time an application call close()
*/
static int dev_flush(struct file *, fl_owner_t id);

#endif
