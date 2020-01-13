# Timed messaging system

## Features
This repository contains the implementation of a Linux Device Driver that allows exchanging messages across threads. Each message posted to the device file is an independent data unit and each read operation can extract the content of a single message (if present). Messages that are already stored in the device file are delivered to readers in FIFO order.

The driver exposes via the `/sys` file system the following parameters:
- `max_message_size`: maximum size in bytes allowed for posting messages to the device file
- `max_storage_size`: maximum number of bytes globally allowed for keeping messages in the device file. If a new message post is requested and such maximum size is already met, then the post must fail.

These parameters can be updated by the root user.

The driver handles a multi-instance device file. Each instance is associated with a specific minor number. The number of supported minors can be configured at compile time by means of macro `MINORS`.

Concurrent I/O sessions on the device file are supported too. Each session can be configured through the `ioctl()` interface. Namely, `ioctl()` can be used to post the following commands:
- `SET_SEND_TIMEOUT`: Upon `write()`, the messages are not stored directly to the device file but after a timeout expressed in milliseconds by the user and then converted in jiffies. Timeout set to the value zero means immediate write. In both cases, immediate and delayed write, the opeartion returns immediately control to the calling thread. By default, the write timeout is 0.
- `SET_RECV_TIMEOUT`: A `read()` operation resumes its execution after a timeout expressed in milliseconds by the user and then converted in jiffies, even if no message is currently present in the device file. Timeout set to zero means non-blocking reads in the absence of messages from the device file. By default, the read timeout is 0.
- `REVOKE_DELAYED_MESSAGES`: Undoes the message-post of messages that have not yet been stored into the device file because their send-timeout is not yet expired.

The driver support the following set of file operations (see `timed-msg-system.h` for further details):
- `open`: Initialize an I/O session on an instance of the device file. It returns 0 on success.
- `unlocked_ioctl()`: Modify the operating mode of `read()` and `write()` as previously described. It returns 0 on success.
- `write()`: Write a message into the device file. On success, it returns 0 if a write timeout exists, the number of written bytes otherwise. If the input message is too long `-EMSGSIZE` is returned, if the device file is full, `-ENOSPC` is returned. Note that when a write is delayed, the message-post operation may fail in the absence of free space in the device file.
- `read()`: Read a message from the device file. It returns the number of read bytes on success. Otherwise, it returns `-ENOMSG` if no message is available and the operating mode is non-blocking and `-ETIME` when the operating mode is blocking and the timeout expires.
- `flush()`: Reset the state of the device file. In more detail, it causes all threads waiting for messages (along any session) to be unblocked (in that case, `read()` returns `-ECANCELED`) and all the delayed messages not yet delivered to be revoked. This function is called every time an application call `close()`.
- `release()`: Release an I/O session on the device file. It is not invoked every time a process calls close. Whenever a `file` structure is shared, it won't be invoked until all copies are closed.

## Internals

### Data structures

Each instance of the device file is represented by a `struct minor_struct`:
```
struct minor_struct {
    unsigned int current_size;
    struct mutex mtx;
    struct list_head fifo;
    struct list_head sessions;
    struct list_head pending_reads;
    wait_queue_head_t read_wq;
};
```
`fifo` is the list of messages currenty stored in the device file. Each message is associated with a `struct message_struct`:
```
struct message_struct {
    unsigned int size;
    char *buf;
    struct list_head list;
}
```
As we can see, for the lists the standard implementation provided by Linux has been used.

The `sessions` field in `struct minor_struct` is the list of sessions currently opened on the device file. Each session is associated with a `struct session_struct`:
```
struct session_struct {
    struct mutex mtx;
    struct workqueue_struct *write_wq;
    unsigned long write_timeout;
    unsigned long read_timeout;
    struct list_head pending_writes;
    struct list_head list;
}
```
`write_timeout` and `read_timeout` are the timeouts discussed above, expressed in jiffies. `write_wq` is the workqueue used for write deferring. All the deferred writes related to the session are stored inside the `pending_writes` list. Each node of the list is a `struct pending_write_struct`:
```
struct pending_write_struct {
  int minor;
  struct session_struct *session;
  char *kbuf;
  unsigned int len;
  struct delayed_work delayed_work;
  struct list_head list;
};
```
`pending_reads` and `read_wq` fields inside `minor_struct` are instead representative of the readers waiting for available messages. Namely, `pending_reads` is a list of `struct pending_read_struct`:

```
struct pending_read_struct {
	int msg_available;
	int flushing;
	struct list_head list;
};

```

### Operations

#### Driver installation
Upon driver installation, an array of `MINOR` `minor_struct` is initialized and the device driver is registered through `__register_chrdev()`. The major number is dinamically allocated by the kernel.

After the installation, you can check the major by typing `dmesg` on the shell. Then, to test the module you can create a corresponding device file. For example...
```
$ sudo mknod timed_test c 236 0
```
... creates a char device file named "timed_test " with major 236 and minor 0.

#### Opening a file
When `open()` is invoked, the driver initializes a `session_struct` object corresponding to the new session. The object is then linked to `struct file` using the field `private_data`. Finally, the `session_struct` is added to the list of open sessions stored by the field `sessions` of the given `minor_struct`.

#### Reading a file
Upon `read()` invocation,  the driver access the list of messages of the device file. If a message is available, it is delivered. Otherwise:
- If the operating mode is non-blocking, `-ENOMSG` is returned.
- If the operating mode is blocking, the thread goes to sleep using `wait_event_interruptible_timeout()` on the `read_wq` waitqueue associated to the device number. Before that, the driver create a new `pending_read_struct` and adds it to the list of pending reads associated to the device file. Different pending readers are associated with different `pending_read_struct`. In that way, selective awakes are possible. In more detail, a reader is awaken if either the `flushing` flag or the `msg_available` flag is set. In the first case, `-ECANCELED` is returned. In the latter case, altough the reader has been awakened by a writer that posted a new message, the reader must check that the list of messages is actually not empty, becasue, due to concurrency, another reader may have been consumed the new message. In that scenario, the reader returns to sleep for the residual amount of jiffies (that the `wait_event_interruptible_timeout` returns when the wait condition becomes true before timer expiration).

#### Writing a file
When `write()` is invoked, the driver check if a write timeout exists. If not so, the message is enqueued in the FIFO associated with the device file and a pending reader, if present, is awaken. Otherwise, a `struct delayed_work` is allocated and passed to the API `queue_delayed_work()` to defer the message-post. The `struct` is embedded inside a `struct pending_write_struct` so that the deferred function can access the needed information by means of `container_of`. Namely, it is necessary using `container_of()` twice, because the input passed to the deferred function is a `struct work_struct` embedded in the `struct delayed_work`.

#### Timeout granularity
Read and write timeout can be configured as seen abouve through `ioctl()`.
For example...
```
ioctl(fd, SET_SEND_TIMEOUT, 20);
```
set a write timeout to the value of 20 milliseconds. The milliseconds input is converted in jiffies using the `HZ` macro contained in `linux/param.h`. This macro represents the number of jiffies per second. Therefore, the conversion requires to multiply the `ioctl` input for `HZ` and divide it by 1000. The value of `HZ` is machine-dependent. Typical values are 100 and 1000.

#### Revoking delayed messages
Invoking `ioctl(fd, REVOKE_DELAYED_MESSAGES)` the deferred writes along a given session are revoked. This is made internally by using the API `cancel_delayed_work()`. This function returns `true` if the canceled work was actually pending, `false` otherwise. The latter return value shows up when a deferred write has not yet completed its execution. `dev_flush()` does not wait for deferred writes like that while `dev_release()` does that as we will see below.

#### Closing a file
Upon `release()` invocation the driver deallocated the `session_struct` instance previously stored by `open()` inside the field `private_data` of `struct file`. Before doing that, the function has to wait for deferred write in execution to terminate. For this purpose, the `flush_workqueue()` API is invoked.

#### Driver uninstallation
When the driver is uninstalled the messages stored in the device files are destroyed and the corresponding buffers deallocated.

