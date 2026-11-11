#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

/* ddr3分配给屏幕的显存大小为32MiB即：lcd->fix.smem_len */
/* 屏幕一帧画面占用的最大内存容量为 1024*600*4=2.34375MiB */
struct lcd_device {
	int fd;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned char *memory;
};

static volatile sig_atomic_t running = 1;

/* 参数signo为信号编号，例如Ctrl + C对应的编号为SIGINT */
static void stop_test(int signo)
{
	/* 知道有这个参数，但当前不用它 */
	(void)signo;
	running = 0;
}

static int lcd_clear(struct lcd_device *lcd)
{
	unsigned int y;
	size_t offset;
	size_t bytes = (size_t)lcd->var.xres * 4;

	for (y = 0; y < lcd->var.yres; y++) {
		offset = ((size_t)y + lcd->var.yoffset) * lcd->fix.line_length +
			(size_t)lcd->var.xoffset * 4;

		memset(lcd->memory + offset, 0, bytes);
	}

	return 0;
}

static int lcd_close(struct lcd_device *lcd)
{
	int ret = 0;

	if (lcd->memory != MAP_FAILED) {
		if (munmap(lcd->memory, lcd->fix.smem_len) < 0) {
			perror("munmap");
			ret = -1;
		}

		lcd->memory = MAP_FAILED;
	}

	if (lcd->fd >= 0) {
		if (close(lcd->fd) < 0) {
			perror("close framebuffer");
			ret = -1;
		}

		lcd->fd = -1;
	}

	return ret;
}

static int lcd_init(struct lcd_device *lcd)
{
	lcd->fd = open("/dev/fb0", O_RDWR);
	if (lcd->fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}

	/* 解除 LCD 的消隐状态，让屏幕恢复显示 */
	if (ioctl(lcd->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		perror("unblank framebuffer");
		goto err;
	}

	if (ioctl(lcd->fd, FBIOGET_VSCREENINFO, &lcd->var) < 0) {
		perror("get variable screen info");
		goto err;
	}

	if (ioctl(lcd->fd, FBIOGET_FSCREENINFO, &lcd->fix) < 0) {
		perror("get fixed screen info");
		goto err;
	}

	/* 将ddr3中framebuffer对应的的物理内存区域映射为应用的虚拟内存空间  */
	lcd->memory = mmap(NULL, lcd->fix.smem_len, PROT_READ | PROT_WRITE,
		MAP_SHARED, lcd->fd, 0);
	if (lcd->memory == MAP_FAILED) {
		perror("mmap framebuffer");
		goto err;
	}

	printf("LCD: %u x %u,%u bpp,line = %u bytes\n", lcd->var.xres,
		lcd->var.yres, lcd->var.bits_per_pixel, lcd->fix.line_length);

	return 0;

err:
	lcd_close(lcd);
	return -1;
}

/* 以(x,y)为中心,画一个7*7像素的蓝色方块 */
static void lcd_draw_point(struct lcd_device *lcd, int x, int y)
{
	unsigned int pixel = 0X000000FF;
	int px;
	int py;
	size_t offset;

	for (py = y - 3; py <= y + 3; py++) {
		for (px = x - 3; px <= x + 3; px++) {
			if (px < 0 || py < 0 || (unsigned int)px >= lcd->var.xres ||
				(unsigned int)py >= lcd->var.yres) {
				continue;
			}

			offset = ((size_t)py + lcd->var.yoffset) * lcd->fix.line_length +
				((size_t)px + lcd->var.xoffset) * 4;

			memcpy(lcd->memory + offset, &pixel, sizeof(pixel));
		}
	}
}

int main(int argc, char *argv[])
{
	struct lcd_device lcd;
	struct input_event event;
	struct input_absinfo abs_x;
	struct input_absinfo abs_y;
	struct pollfd input_poll;
	struct sigaction
		action; /* 收到某个信号时，要调用谁，处理时额外屏蔽哪些信号，以及使用什么选项。 */
	ssize_t ret;
	int ready;
	int fd = -1;
	int result = EXIT_FAILURE;
	int x;
	int y;
	int pressed = 0;

	memset(&lcd, 0, sizeof(lcd));
	lcd.fd = -1;
	lcd.memory = MAP_FAILED; /* 指尚未映射 */

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* 注册信号处理，不设置 SA_RESTART */
	memset(&action, 0, sizeof(action));
	action.sa_handler = stop_test;
	sigemptyset(&action.sa_mask); /* 不额外添加要阻塞的信号 */

	/* 要处理哪个信号 新的处理规则 不需要取回原来的处理规则*/
	if (sigaction(SIGINT, &action, NULL) < 0 ||
		sigaction(SIGTERM, &action, NULL) < 0) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open input");
		return EXIT_FAILURE;
	}

	/* 初始化应用里的坐标变量 */
	if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) < 0) {
		perror("get touch abs_x");
		goto out;
	}

	if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) < 0) {
		perror("get touch abs_y");
		goto out;
	}

	x = abs_x.value;
	y = abs_y.value;

	if (lcd_init(&lcd) < 0) {
		goto out;
	}

	memset(&input_poll, 0, sizeof(input_poll));
	input_poll.fd = fd;
	input_poll.events = POLLIN; /* 关心的事件 */

	printf("Poll touch test; Ctrl + C to clear and exit\n");

	while (running) {
		/* 第二个参数为要监视的文件描述符数量 超时200ms后重新检查退出标志 */
		/* 返回值为有事件的文件描述符数量 */
		ready = poll(&input_poll, 1, 200);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("poll");
			goto out;
		}

		if (!running) {
			break;
		}

		if (ready == 0) {
			continue;
		}

		if (input_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "Input device unavailable\n");
			goto out;
		}

		if (!(input_poll.revents & POLLIN)) {
			continue;
		}

		while (running) {
			/* 非阻塞读取，没有事件时返回 -1，同时 errno = EAGAIN */
			ret = read(fd, &event, sizeof(event));

			if (ret < 0) {
				if (errno == EINTR) {
					continue;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break;
				}

				perror("read");
				goto out;
			}

			if (ret != (ssize_t)sizeof(event)) {
				fprintf(stderr, "Incomplete input event\n");
				goto out;
			}

			if (!running) {
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
					lcd_draw_point(&lcd, x, y);
					printf("TOUCH: x = %d,y = %d\n", x, y);
				} else {
					printf("RELEASED\n");
				}
			}
		}
	}

	result = EXIT_SUCCESS;

out:
	if (!running && lcd.memory != MAP_FAILED) {
		if (lcd_clear(&lcd) < 0) {
			result = EXIT_FAILURE;
		}
	}

	if (lcd_close(&lcd) < 0) {
		result = EXIT_FAILURE;
	}

	if (close(fd) < 0) {
		perror("close input");
		result = -EXIT_FAILURE;
	}

	return result;
}
