#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
/* Use in conjuction with test-kdbus-daemon */

#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "kdbus-util.h"
#include "kdbus-enum.h"

int main(int argc, char *argv[])
{
	int ret;
	char *bus;
	struct conn *conn;
	int serial = 1;
	int fds[3];

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%u-testbus/bus", getuid()) < 0)
		return EXIT_FAILURE;

	conn = connect_to_bus(bus, 0);
	if (!conn)
		return EXIT_FAILURE;

	fds[0] = open("data/file1", O_RDONLY);
	fds[1] = open("data/file2", O_WRONLY);
	fds[2] = open("data/file3", O_RDWR);

	for (i = 0; i < ELEMENTSOF(fds); i++) {
	     fprintf("Unable to open data/fileN file(s)\n");
	     return EXIT_FAILURE;
	}

	ret = msg_send(conn, "com.example.kdbus-test", serial++, 0, 0, 0, 0, 0, 0);
	if (ret < 0)
	     fprintf(stderr, "error sending simple message: %d (%m)\n", ret);

	ret = msg_send(conn, "com.example.kdbus-test", serial++, 0, 0, 0, 0, 1, fds);
	if (ret < 0)
	     fprintf(stderr, "error sending message with 1 fd: %d (%m)\n", ret);

	ret = msg_send(conn, "com.example.kdbus-test", serial++, 0, 0, 0, 0, 2, fds);
	if (ret < 0)
	     fprintf(stderr, "error sending message with 2 fds: %d (%m)\n", ret);

	ret = msg_send(conn, "com.example.kdbus-test", serial++, 0, 0, 0, 0, 3, fds);
	if (ret < 0)
	     fprintf(stderr, "error sending message with 3 fds: %d (%m)\n", ret);

	close(conn->fd);
	free(conn);
	free(bus);

	return EXIT_SUCCESS;
}
