#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

static int lcd_show_info(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd;
	int result = -1;
	unsigned char *fb_memory = MAP_FAILED;
	unsigned int pixel = 0X0000FF00;
	unsigned int x;
	unsigned int y;
	size_t offset;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("get variable screen info");
		goto out;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("get fixed screen info");
		goto out;
	}

	printf("Resolution: %u x %u\n", var.xres, var.yres);
	printf("Virtual resolution: %u x %u\n", var.xres_virtual, var.yres_virtual);
	printf("Bits per pixel: %u\n", var.bits_per_pixel);
	printf("Line length: %u bytes\n", fix.line_length);
	printf("Framebuffer memory: %u bytes\n", fix.smem_len);

	printf("Red:   offset=%u, length=%u\n", var.red.offset, var.red.length);
	printf("Green: offset=%u, length=%u\n", var.green.offset, var.green.length);
	printf("Blue:  offset=%u, length=%u\n", var.blue.offset, var.blue.length);
	printf("xoffset=%u, yoffset=%u\n", var.xoffset, var.yoffset);

	/* 将显存映射到应用的虚拟地址空间 */
	fb_memory =
		mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb_memory == MAP_FAILED) {
		perror("mmap framebuffer");
		goto out;
	}

	for (y = var.yres / 2 - 10; y < var.yres / 2 + 10; y++) {
		for (x = var.xres / 2 - 10; x < var.xres / 2 + 10; x++) {
			offset = ((size_t)y + var.yoffset) * fix.line_length +
				((size_t)x + var.xoffset) * 4;
			if (offset > fix.smem_len ||
				sizeof(pixel) > fix.smem_len - offset) {
				fprintf(stderr, "Pixel exceeds framebuffer memory\n");
				goto out;
			}

			memcpy(fb_memory + offset, &pixel, sizeof(pixel));
		}
	}

	result = 0;

out:
	if (fb_memory != MAP_FAILED) {
		if (munmap(fb_memory, fix.smem_len) < 0) {
			perror("munmap");
			result = -1;
		}
	}

	if (close(fd) < 0) {
		perror("close /dev/fb0");
		result = -1;
	}

	return result;
}

int main(int argc, char *argv[])
{
	struct input_event event;
	ssize_t ret;
	int fd;
	int result = EXIT_SUCCESS;
	int x = 0;
	int y = 0;
	int pressed = 0;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (lcd_show_info() < 0) {
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

		if (event.type == EV_ABS) {
			if (event.code == ABS_X) {
				x = event.value;
			} else if (event.code == ABS_Y) {
				y = event.value;
			}
		} else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
			pressed = event.value != 0;
		} else if (event.type == EV_SYN && event.code == SYN_REPORT) {
			if (pressed) {
				printf("TOUCH: x = %d,y = %d\n", x, y);
			} else {
				printf("RELEASED\n");
			}
		}
	}

	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}

	return result;
}