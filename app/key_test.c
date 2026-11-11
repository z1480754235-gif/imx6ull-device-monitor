#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

int main(int argc, char *argv[])
{
	struct input_event event;
	const char *state;
	ssize_t ret;
	int fd;
	int result = EXIT_SUCCESS;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	printf("Listening on %s;press Ctrl+C to exit\n", argv[1]);

	while (1) {
		ret = read(fd, &event, sizeof(event));

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

		if (ret != (ssize_t)sizeof(event)) {
			fprintf(stderr, "read: incomplete input event\n");
			result = EXIT_FAILURE;
			break;
		}

		if (event.type != EV_KEY) {
			continue;
		}

		switch (event.value) {
		case 0:
			state = "RELEASED";
			break;
		case 1:
			state = "PRESSED";
			break;
		case 2:
			state = "REPEAT";
			break;
		default:
			continue;
		}

		printf("key code = %u,%s\n", (unsigned int)event.code, state);
	}

	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}

	return result;
}