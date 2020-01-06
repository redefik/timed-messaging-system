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
	unsigned int major, timeout;
	int ret, fd;
	char msg[MAX_MSG_SIZE];

	if (argc != 5) {
		fprintf(stderr, "Usage:sudo %s <pathname> <major> <msecs> <message>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	timeout = strtoul(argv[3], NULL, 0); //mseconds
		
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
	
	printf("Setting write timeout...\n");
	
	// Set write timeout
	ioctl(fd, SET_SEND_TIMEOUT, timeout);
	
	printf("Writing the input message...\n");
	// Write the input message
	ret = write(fd, argv[4], strlen(argv[4]) + 1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		return(EXIT_FAILURE);
	}

	printf("Sleeping...\n");
	usleep((2*timeout)*1000); // sleep to let the message be written
	
	printf("Reading...\n");
	// Read the message
	ret = read(fd, msg, MAX_MSG_SIZE);
	if (ret <= 0) {
		fprintf(stderr, "read failed in an unexpected way\n");
		return(EXIT_FAILURE);
	}
	printf("read: %s\n", msg);
	
	printf("Writing the input message...\n");
	ret = write(fd, argv[4], strlen(argv[4]) + 1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		return(EXIT_FAILURE);
	}
	
	printf("Revoking delayed write...\n");
	ioctl(fd, REVOKE_DELAYED_MESSAGES);
	
	printf("Sleeping...\n");
	usleep((2*timeout)*1000);
		
	printf("Reading...\n");
	ret = read(fd, msg, MAX_MSG_SIZE);
	if (ret == -1 && errno == EAGAIN) {
		printf("read() returned EAGAIN as expected\n");
		return(EXIT_SUCCESS);
	}

	printf("Unexpected behaviour of read(), return value=%d\n", ret);
	return(EXIT_FAILURE);

}
