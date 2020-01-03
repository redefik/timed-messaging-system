#include <linux/ioctl.h>

#define MAGIC_BASE 'k' // TODO it should be unique across the system
#define SET_SEND_TIMEOUT _IO(MAGIC_BASE, 0)
#define SET_RECV_TIMEOUT _IO(MAGIC_BASE, 1)
#define REVOKE_DELAYED_MESSAGES _IO(MAGIC_BASE, 2)
