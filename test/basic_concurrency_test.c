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

#define MINOR 0
#define WRITERS 10
#define READERS 2
#define MAX_MSG_SIZE 128

// Compile with -lpthread
// Execute after sudoing in your shell

int fd;

void *writer(void *arg)
{
	pthread_t id;
	char msg[MAX_MSG_SIZE];
	int ret;
	
	// Post two messages
	 
	id = pthread_self();
	sprintf(msg, "%lu-in\n", id);
	
	ret = write(fd, msg, strlen(msg)+1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		exit(EXIT_FAILURE);
	}
	
	sprintf(msg, "%lu-out\n", id);
	ret = write(fd, msg, strlen(msg)+1);
	if (ret == -1) {
		fprintf(stderr, "write() failed\n");
		exit(EXIT_FAILURE);
	}
	
	return NULL;
	
}

void *reader(void *arg)
{
	char msg[MAX_MSG_SIZE];
	int ret;
	pthread_t id = pthread_self();
	
	// Read messages from the device file
	while (1) {
		ret = read(fd, msg, MAX_MSG_SIZE);
		if (ret == -1 && errno != EAGAIN) {
			fprintf(stderr, "read() failed\n");
			exit(EXIT_FAILURE);
		}
		if (errno != EAGAIN) {
			printf("%lu read: %s\n", id, msg);
		} // the thread will block when the device file becomes empty	
	}
	
	return NULL; // never reached
}

int main(int argc, char *argv[])
{
	int ret, i;
	unsigned int major;
	pthread_t r_tid[READERS];
	pthread_t w_tid[WRITERS];

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <pathname> <major>\n", argv[0]);
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
