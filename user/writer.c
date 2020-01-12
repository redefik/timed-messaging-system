#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include "../timed-msg-system.h"

#define MAX_MSG_SIZE 128

int main(int argc, char *argv[])
{
	int fd ,ret;
	char msg[MAX_MSG_SIZE];
	unsigned long write_timeout;

	if (argc != 4) {
		fprintf(stderr, "Usage: sudo %s <filename> <write_timeout> <manual/auto>\n",
		        argv[0]);
		return(EXIT_FAILURE);
	}
	
	printf("pid: %d\n", getpid());

	
	// Open the input file
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "open() failed: %s\n", strerror(errno));
		return(EXIT_FAILURE);
	}
	
	// Set the write timeout
	write_timeout = strtoul(argv[2], NULL, 0);
	ret = ioctl(fd, SET_SEND_TIMEOUT, write_timeout);
	if (ret == -1) {
		fprintf(stderr, "ioctl() failed: %s\n", strerror(errno));
		return(EXIT_FAILURE);
	}
	
	
	if (strcmp(argv[3],"auto") == 0) { // Automatic, a message is written every second
		srandom(time(NULL));
		while (1) {
			sprintf(msg, "%ld", random());
			ret = write(fd, msg, strlen(msg) +1);
			if (ret == -1) {
				fprintf(stderr, "write() failed: %s\n", strerror(errno));
				return(EXIT_FAILURE);
			}
			//sleep(1);
		}
	}
	
	// Manual, Wait for user's input
	while (1) {
		fputc('>', stdout);
		fflush(stdout);
		if (!fgets(msg, MAX_MSG_SIZE, stdin)) {
			fprintf(stderr, "fgets() failed\n");
			return(EXIT_FAILURE);
		}
		msg[strlen(msg)-1] = '\0';
		if (strcmp(msg, "REVOKE_DELAYED_MESSAGES") == 0) {
			ret = ioctl(fd, REVOKE_DELAYED_MESSAGES);
			if (ret == -1) {
				fprintf(stderr, "revoke delayed messages failed: %s\n", strerror(errno));
			} else {
				printf("delayed messages have been revoked\n");
			}
			continue;
		}
		if (strcmp(msg, "CLOSE") == 0) {
			close(fd);
			printf("File descriptor closed\n");
			return(EXIT_SUCCESS);
		}
		// Write the input into the device file
		ret = write(fd, msg, strlen(msg) + 1);
		if (ret == -1) {
			fprintf(stderr, "write() failed: %s\n", strerror(errno));
		} else {
			fprintf(stderr, "write() returned %d\n", ret);
		}
	}
	
}
