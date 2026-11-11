#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "../include/ap3216c_data.h"

int main(int argc, char *argv[])
{
	struct ap3216c_data data;
	ssize_t ret;
	int fd;
	int result = EXIT_SUCCESS;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/ap3216c\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	printf("Reading AP3216C;press Ctrl + C to exit\n");

	while (1) {
		ret = read(fd, &data, sizeof(data));

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("read");
			result = EXIT_FAILURE;
			break;
		}

		if (ret == 0) {
			break;
		}

		if (ret != (ssize_t)sizeof(data)) {
			fprintf(stderr, "read: incomplete sensor data\n");
			result = EXIT_FAILURE;
			break;
		}

		printf("IR=%hu (%s),ALS=%hu (%s),PS=%hu (%s)\n", data.ir,
			(data.flags & AP3216C_IR_VALID) ? "valid" : "invalid", data.als,
			(data.flags & AP3216C_ALS_VALID) ? "valid" : "invalid", data.ps,
			(data.flags & AP3216C_PS_VALID) ? "valid" : "invalid");

		sleep(1);
	}

	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}

	return result;
}