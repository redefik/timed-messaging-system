#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../timed-msg-system.h"

#define MAX_MSG_SIZE 128

int main(int argc, char *argv[])
{
	int fd, ret;
	unsigned long read_timeout;
	char msg[MAX_MSG_SIZE];
	
	if (argc != 3) {
		fprintf(stderr, "Usage: sudo %s <filename> <read_timeout>\n", 
		        argv[0]);
		return(EXIT_FAILURE);
	}
	
	printf("pid: %d\n", getpid());
	
	// Open the device file
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "open() failed: %s\n", strerror(errno));
		return(EXIT_FAILURE);
	}
	
	// Set Read Timeout
	read_timeout = strtoul(argv[2], NULL, 0);
	ret = ioctl(fd, SET_RECV_TIMEOUT, read_timeout);
	if (ret == -1) {
		fprintf(stderr, "ioctl() failed: %s\n", strerror(errno));
		return(EXIT_FAILURE);
	}
	
	// Read from the file
	while (1) {
		ret = read(fd, msg, MAX_MSG_SIZE);
		if (ret == -1) {
			fprintf(stderr, "read() failed: %s\n", strerror(errno));
		} else {
			printf("read: %s\n", msg);
		}
	}
	
}
