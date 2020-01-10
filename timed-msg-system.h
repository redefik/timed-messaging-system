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

/* Supported File Operations*/

/**
* dev_open - Initialize an I/O session to the device file
* 
* @inodep: pointer to %struct inode representing the device file
* @filep: pointer to %struct file associated to the I/O session
*
* Returns %0 on success, %-ENOMEM if it fails in allocating auxiliary data
* structures
* 
* This function initialize a %session_struct object and link it to struct file
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
* - %-EAGAIN if no message is available and the operating mode of
*   the I/O session is non-blocking (read timeout equal to 0)
* - %-ENOMEM if it fails in allocating a %pending_read_struct
* - %-ERESTARTSYS if the blocking read is interrupted by a signal
* - %-ECANCELED if during a blocking read someone reset the state of the 
*   device file through dev_flush()
* - %-EFAULT if the provided buffer is illegal   
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
 * - %EAGAIN if the device file is temporary full
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
* dev_flush - Reset the state of the device file+
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
