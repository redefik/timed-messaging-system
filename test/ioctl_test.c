#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include "../timed-msg-system.h"

// Execute after sudoing in your shell

#define MINOR 0

int main(int argc, char *argv[])
{
	unsigned int major;
	int ret, fd;

	if (argc != 3) {
		fprintf(stderr, "Usage:%s <pathname> <major>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	
	// Create a char device file with the given major and 0 with minor number
	ret = mknod(argv[1], S_IFCHR, makedev(major, MINOR));
	if (ret == -1) {
		fprintf(stderr, "mknod() failed\n");
		return(EXIT_FAILURE);
	}
	
	// Open the file
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "open() failed\n");
		return(EXIT_FAILURE);
	}
	
	// Perform various ioctl
	ioctl(fd, SET_SEND_TIMEOUT, 0);
	ioctl(fd, SET_RECV_TIMEOUT, 1);
	ioctl(fd, REVOKE_DELAYED_MESSAGES);
	ioctl(fd, 12345);
	
	printf("Check dmesg\n");
	
	return(EXIT_SUCCESS);
}
