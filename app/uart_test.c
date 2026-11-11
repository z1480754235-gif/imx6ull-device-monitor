#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

static int uart_setup(int fd, const struct termios *old)
{
	struct termios options = *old;

	/* 关闭字符转换、输出处理、回显和按行读取 */
	options.c_iflag = 0;
	options.c_oflag = 0;
	options.c_lflag = 0;

	/* 8 数据位、无校验、1 停止位、无硬件流控 */
	options.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
	options.c_cflag |= CS8 | CLOCAL | CREAD;

	/* 设置接收和发送波特率 */
	if (cfsetispeed(&options, B115200) < 0 ||
		cfsetospeed(&options, B115200) < 0) {
		return -1;
	}

	/* 每次 read 最多等待 2 秒，收到数据就可以返回 */
	options.c_cc[VMIN] = 0;
	options.c_cc[VTIME] = 20;

	/* 测试前清除旧的收发数据 */
	if (tcflush(fd, TCIOFLUSH) < 0) {
		return -1;
	}

	/* 立即应用配置 */
	return tcsetattr(fd, TCSANOW, &options);
}

int main(int argc, char *argv[])
{
	const char tx[] = "hello";
	char rx[sizeof(tx)] = { 0 }; /* sizeof(tx) = 6 */
	const size_t length = sizeof(tx) - 1; /* length = 5 */
	struct termios old;
	size_t sent = 0;
	size_t received = 0;
	ssize_t ret;
	int fd;
	int configured = 0;
	int result = EXIT_FAILURE;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/ttymxc2\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	/* 保存原来的串口配置，退出时恢复 */
	if (tcgetattr(fd, &old) < 0) {
		perror("tcgetattr");
		goto out;
	}

	if (uart_setup(fd, &old) < 0) {
		perror("uart_setup");
		goto out;
	}
	configured = 1;

	/* 串口是字节流，一次 read() 或 write() 返回的字节数小于请求数量 */
	while (sent < length) {
		ret = write(fd, tx + sent, length - sent);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("write");
			goto out;
		}

		if (ret == 0) {
			fprintf(stderr, "write: no progress\n");
			goto out;
		}

		sent += (size_t)ret;
	}

	printf("TX: %s\n", tx);

	while (received < length) {
		ret = read(fd, rx + received, length - received);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("read");
			goto out;
		}

		if (ret == 0) {
			fprintf(stderr, "RX timeout: received %zu/%zu bytes\n", received,
				length);
			goto out;
		}

		received += (size_t)ret;
	}

	printf("RX: %s\n", rx);

	if (memcmp(tx, rx, length) != 0) {
		fprintf(stderr, "Loopback Fail: data mismatched\n");
		goto out;
	}

	printf("Loopback Pass\n");
	result = EXIT_SUCCESS;

out:
	if (configured &&
		tcsetattr(fd, TCSANOW, &old) < 0) /* 退出时恢复原来的配置 old */
	{
		perror("restore termious");
		result = EXIT_FAILURE;
	}

	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}

	return result;
}