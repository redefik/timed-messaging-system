#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "../timed-msg-system.h"

// Execute after sudoing in your shell
// Execute with time

#define MINOR 0
#define MAX_MSG_SIZE 128

int main(int argc, char *argv[])
{
	unsigned int major, timeout;
	int ret, fd;
	char msg[MAX_MSG_SIZE];

	if (argc != 5) {
		fprintf(stderr, "Usage:%s <pathname> <major> <timeout> <message>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	timeout = strtoul(argv[3], NULL, 0);
	
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
	
	// Set write timeout
	ioctl(fd, SET_SEND_TIMEOUT, timeout);
	
	// Write the input message
	ret = write(fd, argv[4], strlen(argv[4]) + 1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		return(EXIT_FAILURE);
	}
	printf("write done, return value:%d\n", ret);
	printf("Reading...\n");
	
	// Read the message
	while (1) {
		ret = read(fd, msg, MAX_MSG_SIZE);
		if (ret == -1 && errno != EAGAIN) {
			fprintf(stderr, "read() failed\n");
			return(EXIT_FAILURE);
		}
		if (ret > 0) {
			printf("read: %s\n", msg);
			break;
		}
	}
	
	return(EXIT_SUCCESS);
}
