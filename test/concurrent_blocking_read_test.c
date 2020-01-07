#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include "../timed-msg-system.h"

#define MINOR 0
#define WRITERS 1
#define READERS 2
#define MAX_MSG_SIZE 128

// Compile with -lpthread
// Execute after sudoing in your shell
// NOTE Before executing flush the device file to observe the expected
// behaviour

int fd;

void *writer(void *arg)
{
	pthread_t id;
	char msg[MAX_MSG_SIZE];
	int ret;
	
	// Post one message
	 
	id = pthread_self();
	sprintf(msg, "%lu-in\n", id);
	
	ret = write(fd, msg, strlen(msg)+1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		exit(EXIT_FAILURE);
	}
	
	/*sprintf(msg, "%lu-out\n", id);
	ret = write(fd, msg, strlen(msg)+1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		exit(EXIT_FAILURE);
	}*/
	
	return NULL;
	
}

void *reader(void *arg)
{
	char msg[MAX_MSG_SIZE];
	int ret;
	int pid = syscall(186); //gettid() system call (not wrapped by glibc currently)
	
	// Read messages from the device file
	ret = read(fd, msg, MAX_MSG_SIZE);
	if (ret == -1 && errno == ETIME) {
		printf("%d timeout expired\n", pid);
		return NULL;
	}
	if (ret == -1 & errno != ETIME) {
		printf("%d read() failed in a strange way\n", pid);
		return NULL;
	}
	if (ret > 0) {
		printf("%d read: %s\n", pid, msg);
		return NULL;
	}	
}

int main(int argc, char *argv[])
{
	int ret, i;
	unsigned int major;
	pthread_t r_tid[READERS];
	pthread_t w_tid[WRITERS];
	unsigned long r_timeout, w_timeout;

	if (argc != 5) {
		fprintf(stderr, "Usage: %s <pathname> <major> <r_timeout> <w_timeout>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	r_timeout = strtoul(argv[3], NULL, 0);
	w_timeout = strtoul(argv[4], NULL, 0);

	
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
	
	ret = ioctl(fd, SET_SEND_TIMEOUT, (unsigned long)w_timeout);
	if (ret == -1) {
		fprintf(stderr, "ioctl() failed\n");
		return(EXIT_FAILURE);
	}
	
	ret = ioctl(fd, SET_RECV_TIMEOUT, (unsigned long)r_timeout);
	if (ret == -1) {
		fprintf(stderr, "ioctl() failed\n");
		return(EXIT_FAILURE);
	}
	
	if (w_timeout >= r_timeout) {
		printf("Expected: both readers will fail to read because of timer expiration\n");
	} else {
		printf("Expected: Only a reader will succeed in the read operation\n");
	}
	
	printf("Actual behaviour\n");
	
	// Create readers and writers
	for (i = 0; i < WRITERS; i++) {
		if (pthread_create(&w_tid[i], NULL, writer, NULL)) {
			fprintf(stderr, "pthread_create() failed\n");
			return(EXIT_FAILURE);
		}
	}
	
	for (i = 0; i < READERS; i++) {
		if (pthread_create(&r_tid[i], NULL, reader, NULL)) {
			fprintf(stderr, "pthread_create() failed\n");
			return(EXIT_FAILURE);
		}
	}
	
	while(1);
	
	return(EXIT_SUCCESS); // never reached
}
