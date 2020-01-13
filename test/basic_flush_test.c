#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/sysmacros.h>
#include "../timed-msg-system.h"

// Execute after sudoing
// Ensure that the device file instance is empty to observe the expected behaviour

#define MINOR 0
#define MAX_MSG_SIZE 128

int main(int argc, char *argv[])
{
	unsigned int major;
	unsigned long write_timeout;
	char msg[] = "test";
	char buf[MAX_MSG_SIZE];
	int pid, ret, fd;

	if (argc != 4) {
		fprintf(stderr, "Usage: sudo %s <pathname> <major> <write_timeout>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	major = strtoul(argv[2], NULL, 0);
	write_timeout = strtoul(argv[3], NULL, 0);
	
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
	// Setting write timeout
	ret = ioctl(fd, SET_SEND_TIMEOUT, write_timeout);
	if (ret == -1) {
		fprintf(stderr, "ioctl() failed\n");
		return(EXIT_FAILURE);
	}
	// Writing a message
	ret = write(fd, msg, strlen(msg)+1);
	if (ret != 0) {
		fprintf(stderr, "write() unexpectedly returned %d\n", ret);
		return(EXIT_FAILURE);
	}
	// Fork a child that cause flush() to be invoked
	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "fork() failed\n");
		return(EXIT_FAILURE);
	}
	if (pid == 0) { // child
		close(fd);
		return(EXIT_SUCCESS);
	}
	// Wait for child termination
	ret = wait(NULL);
	if (ret == -1) {
		fprintf(stderr, "wait() failed");
		return(EXIT_FAILURE);
	}
	usleep((2*write_timeout)*1000);
	// Read - ENOMSG expected
	ret = read(fd, buf, MAX_MSG_SIZE);
	if (ret == -1 && errno == ENOMSG) {
		printf("read() returned ENOMSG as expected\n");
		close(fd);
		return(EXIT_SUCCESS);
	} else {
		printf("read() returned: %s\n", strerror(errno));
		return(EXIT_FAILURE);
	}
	
}
