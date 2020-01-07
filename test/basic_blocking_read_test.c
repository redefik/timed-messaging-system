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
#include <sys/time.h>
#include "../timed-msg-system.h"

// Execute after sudoing in your shell

#define MINOR 0
#define MAX_MSG_SIZE 128


int main(int argc, char *argv[])
{
	unsigned int major, r_timeout, w_timeout;
	int ret, fd;
	char msg[MAX_MSG_SIZE];

	if (argc != 6) {
		fprintf(stderr, "Usage:sudo %s <pathname> <major> <read-timeout> <write-timeout> <message>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	r_timeout = strtoul(argv[3], NULL, 0); // mseconds
	w_timeout = strtoul(argv[4], NULL, 0); // mseconds
		
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
	
	printf("Setting timeout...\n");
	
	// Set read timeout
	ioctl(fd, SET_RECV_TIMEOUT, r_timeout);
	// Set write timeout
	ioctl(fd, SET_SEND_TIMEOUT, w_timeout);
	
	printf("Writing the input message...\n");
	// Write the input message
	ret = write(fd, argv[5], strlen(argv[5]) + 1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		return(EXIT_FAILURE);
	}
	
	printf("Reading...\n");
	// Read the message
	ret = read(fd, msg, MAX_MSG_SIZE);
	if (w_timeout >= r_timeout) {
		if (ret == -1 && errno == ETIME) {
			printf("read() returned -1 with errno ETIME as expected\n");
			return(EXIT_SUCCESS);
		} else {
			printf("read() returned %d\n - unexpected", ret);
			return(EXIT_FAILURE);
		} 
	} else {
		if (ret > 0) {
			printf("read: %s as expected\n", msg);
			return(EXIT_SUCCESS);
		} else {
			printf("read() returned %d\n - unexpected", ret);
			return(EXIT_FAILURE);
		}
	}
}
