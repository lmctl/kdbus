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

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%u-testbus/bus", getuid()) < 0)
		return EXIT_FAILURE;

	conn = connect_to_bus(bus, 0);
	if (!conn)
		return EXIT_FAILURE;

	ret = msg_send(conn, "com.example.kdbus-test", 0xc0000000, 0, 0, 0, 0, 0, 0);
	printf("msg_sent returned %d (errno=%d/%s)\n", ret, errno, strerror(errno));

	close(conn->fd);
	free(conn);
	free(bus);

	return EXIT_SUCCESS;
}
