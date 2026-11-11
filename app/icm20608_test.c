#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "../include/icm20608_data.h"

int main(int argc, char *argv[])
{
	struct icm20608_data data;
	ssize_t ret;
	int fd;
	int result = EXIT_SUCCESS;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/icm20608\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	printf("Reading ICM20608;press Ctrl + C to exit\n");

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

		printf("ACC raw: %hd %hd %hd | "
			   "GYRO raw: %hd %hd %hd | TEMP raw: %hd\n",
			data.accel_x, data.accel_y, data.accel_z, data.gyro_x, data.gyro_y,
			data.gyro_z, data.temp);

		printf("ACC(g): %.3f %.3f %.3f | "
			   "GYRO(deg/s): %.3f %.3f %.3f\n",
			data.accel_x / ICM20608_ACCEL_LSB_PER_G,
			data.accel_y / ICM20608_ACCEL_LSB_PER_G,
			data.accel_z / ICM20608_ACCEL_LSB_PER_G,
			data.gyro_x / ICM20608_GYRO_LSB_PER_DPS,
			data.gyro_y / ICM20608_GYRO_LSB_PER_DPS,
			data.gyro_z / ICM20608_GYRO_LSB_PER_DPS);

		sleep(1);
	}

	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}

	return result;
}