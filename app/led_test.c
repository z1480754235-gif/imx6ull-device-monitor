#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	int fd;
	char value;
	ssize_t ret;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <device> <0|1>\n", argv[0]);
		return EXIT_FAILURE;
	}

	value = argv[2][0];

	fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	ret = write(fd, &value, sizeof(value));
	if (ret != (ssize_t)sizeof(value)) {
		if (ret < 0) {
			perror("write");
		} else {
			fprintf(stderr, "write: incomplete write\n");
		}
		close(fd);
		return EXIT_FAILURE;
	}

	if (close(fd) < 0) {
		perror("close");
		return EXIT_FAILURE;
	}

	printf("LED %s\n", value == '1' ? "ON" : "OFF");

	return EXIT_SUCCESS;
}