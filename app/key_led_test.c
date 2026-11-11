#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

static int led_set_state(int fd, int state)
{
	char value = state ? '1' : '0';
	ssize_t ret;

	do {
		ret = write(fd, &value, sizeof(value));
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		return -1;
	}

	if (ret != (ssize_t)sizeof(value)) {
		errno = EIO;
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct input_event event;
	ssize_t ret;
	int fd_led = -1;
	int fd_key = -1;
	int led_state = 0;
	int next_state;
	int result = EXIT_FAILURE;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <input device> <LED device>\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd_key = open(argv[1], O_RDONLY);
	if (fd_key < 0) {
		perror("open key");
		goto out;
	}

	fd_led = open(argv[2], O_WRONLY);
	if (fd_led < 0) {
		perror("open LED");
		goto out;
	}

	if (led_set_state(fd_led, led_state) < 0) {
		perror("initialize LED");
		goto out;
	}

	printf("LED OFF;press the key to toggle,Ctrl+C to exit\n");

	while (1) {
		ret = read(fd_key, &event, sizeof(event));

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("read key");
			goto out;
		}

		if (ret == 0) {
			result = EXIT_SUCCESS;
			goto out;
		}

		if (ret != (ssize_t)sizeof(event)) {
			fprintf(stderr, "read: incomplete input event\n");
			goto out;
		}

		if (event.type != EV_KEY || event.code != KEY_ENTER ||
			event.value != 1) {
			continue;
		}

		next_state = !led_state;
		if (led_set_state(fd_led, next_state) < 0) {
			perror("write LED");
			goto out;
		}

		led_state = next_state;

		printf("LED %s\n", led_state ? "ON" : "OFF");
	}

out:
	if (fd_led >= 0 && close(fd_led) < 0) {
		perror("close LED");
		result = EXIT_FAILURE;
	}

	if (fd_key >= 0 && close(fd_key) < 0) {
		perror("close key");
		result = EXIT_FAILURE;
	}

	return result;
}