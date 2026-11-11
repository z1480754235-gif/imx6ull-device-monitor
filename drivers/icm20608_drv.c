#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#include "../include/icm20608_data.h"

#define ICM20608_NAME  "icm20608"
#define ICM20608_COUNT 1

#define ICM_SMPLRT_DIV	  0x19
#define ICM_CONFIG		  0x1a
#define ICM_GYRO_CONFIG	  0x1b
#define ICM_ACCEL_CONFIG  0x1c
#define ICM_ACCEL_CONFIG2 0x1d
#define ICM_LP_MODE_CFG	  0x1e
#define ICM_FIFO_EN		  0x23
#define ICM_INT_ENABLE	  0x38
#define ICM_ACCEL_XOUT_H  0x3b
#define ICM_USER_CTRL	  0x6a
#define ICM_PWR_MGMT_1	  0x6b
#define ICM_PWR_MGMT_2	  0x6c
#define ICM_WHO_AM_I	  0x75

struct icm20608_device {
	struct spi_device *spi;
	dev_t devid;
	struct cdev *cdev;
	struct class *class;
	struct device *device;
};

static struct icm20608_device icmdev;
static DEFINE_MUTEX(icm20608_lock);

static int icm20608_read_regs(struct spi_device *spi, unsigned char reg,
	void *buf, unsigned int len)
{
	/* 最高位置 1 表示读操作 */
	unsigned char command = reg | 0x80;

	/* 发送1字节地址，再接收len字节数据，中途保持片选有效 */
	return spi_write_then_read(spi, &command, 1, buf, len);
}

static int icm20608_read_reg(struct spi_device *spi, unsigned char reg)
{
	unsigned char value;
	int ret;

	ret = icm20608_read_regs(spi, reg, &value, 1);
	if (ret < 0) {
		return ret;
	}

	return value;
}

static int icm20608_write_reg(struct spi_device *spi, unsigned char reg,
	unsigned char value)
{
	unsigned char tx[2];

	tx[0] = reg & 0x7f;
	tx[1] = value;

	/* 此接口内部使用适合 SPI 传输的缓冲区，地址和数据在一次消息中发送*/
	return spi_write_then_read(spi, tx, sizeof(tx), NULL, 0);
}

static void icm20608_sleep(struct spi_device *spi)
{
	int ret;

	ret = icm20608_write_reg(spi, ICM_PWR_MGMT_1, 0x40);
	if (ret < 0) {
		dev_warn(&spi->dev, "sleep failed: %d\n", ret);
	}
}

/* icm20608 初始化 */
static int icm20608_hw_init(struct spi_device *spi)
{
	static const unsigned char settings[][2] = {
		{ ICM_USER_CTRL, 0x10 }, /* 禁用 I2C 接口 */
		{ ICM_PWR_MGMT_1, 0x01 }, /* 唤醒、选择时钟 */
		{ ICM_PWR_MGMT_2, 0x00 }, /* 启用所有轴 */
		{ ICM_CONFIG, 0x04 }, /* 陀螺仪低通滤波 */
		{ ICM_SMPLRT_DIV, 0x09 }, /* 采样分频 */
		{ ICM_GYRO_CONFIG, 0x18 }, /* ±2000°/s */
		{ ICM_ACCEL_CONFIG, 0x18 }, /* ±16g */
		{ ICM_ACCEL_CONFIG2, 0x04 }, /* 加速度低通滤波 */
		{ ICM_LP_MODE_CFG, 0x00 }, /* 关闭低功耗模式 */
		{ ICM_FIFO_EN, 0x00 }, /* 不使用 FIFO */
		{ ICM_INT_ENABLE, 0x00 }, /* 当前由应用定时读取 */
	};

	unsigned int i;
	int ret;

	/* 复位icm20608 */
	ret = icm20608_write_reg(spi, ICM_PWR_MGMT_1, 0x80);
	if (ret < 0) {
		return ret;
	}

	msleep(100);

	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		ret = icm20608_write_reg(spi, settings[i][0], settings[i][1]);
		if (ret < 0) {
			return ret;
		}
	}

	/* 等待传感器启动稳定 */
	msleep(100);

	ret = icm20608_read_reg(spi, ICM_PWR_MGMT_1);
	if (ret < 0) {
		return ret;
	}
	if (ret != 0x01) {
		return -EIO;
	}

	ret = icm20608_read_reg(spi, ICM_ACCEL_CONFIG);
	if (ret < 0) {
		return ret;
	}
	if (ret != 0x18) {
		return -EIO;
	}

	ret = icm20608_read_reg(spi, ICM_GYRO_CONFIG);
	if (ret < 0) {
		return ret;
	}
	if (ret != 0x18) {
		return -EIO;
	}

	return 0;
}

static signed short icm20608_be16(const unsigned char *p)
{
	/* icm20608原始数据使用 16 位补码表示且为数据存储方式为大端序 */
	return (signed short)(((unsigned short)p[0] << 8) | p[1]);
}

/* 开始连续读取 14 字节 */
static int icm20608_read_raw(struct spi_device *spi, struct icm20608_data *data)
{
	unsigned char buf[14];
	int ret;

	ret = icm20608_read_regs(spi, ICM_ACCEL_XOUT_H, buf, sizeof(buf));
	if (ret < 0) {
		return ret;
	}

	data->accel_x = icm20608_be16(&buf[0]);
	data->accel_y = icm20608_be16(&buf[2]);
	data->accel_z = icm20608_be16(&buf[4]);

	data->temp = icm20608_be16(&buf[6]);

	data->gyro_x = icm20608_be16(&buf[8]);
	data->gyro_y = icm20608_be16(&buf[10]);
	data->gyro_z = icm20608_be16(&buf[12]);

	return 0;
}

static int icm20608_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	mutex_lock(&icm20608_lock);
	if (!icmdev.spi || inode->i_cdev != icmdev.cdev) {
		ret = -ENODEV;
		goto out_lock;
	}

	filp->private_data = &icmdev;

out_lock:
	mutex_unlock(&icm20608_lock);
	return ret;
}

static ssize_t icm20608_read(struct file *filp, char __user *buf, size_t cnt,
	loff_t *offt)
{
	struct icm20608_device *dev = filp->private_data;
	struct icm20608_data data;
	int ret;

	if (cnt == 0) {
		return 0;
	}

	if (cnt < sizeof(data)) {
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&icm20608_lock);
	if (ret) {
		return ret;
	}

	if (!dev->spi || file_inode(filp)->i_cdev != dev->cdev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = icm20608_read_raw(dev->spi, &data);
	if (ret) {
		goto out_unlock;
	}

	mutex_unlock(&icm20608_lock);

	if (copy_to_user(buf, &data, sizeof(data))) {
		return -EINVAL;
	}

	return sizeof(data);

out_unlock:
	mutex_unlock(&icm20608_lock);
	return ret;
}

static const struct file_operations icm20608_fops = {
	.owner = THIS_MODULE,
	.open = icm20608_open,
	.read = icm20608_read,
	.llseek = no_llseek,
};

static int icm20608_probe(struct spi_device *spi)
{
	int ret;
	int chip_id;

	mutex_lock(&icm20608_lock);

	if (icmdev.spi) {
		ret = -EBUSY;
		goto out_unlock;
	}

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;

	/* 让 SPI 控制器驱动检查并应用这些设置 */
	ret = spi_setup(spi);
	if (ret) {
		goto out_unlock;
	}

	chip_id = icm20608_read_reg(spi, ICM_WHO_AM_I);
	if (chip_id < 0) {
		ret = chip_id;
		goto out_unlock;
	}
	if (chip_id != 0xaf && chip_id != 0xae) {
		dev_err(
			&spi->dev, "unexpected WHO_AM_I: 0x%02x\n", (unsigned int)chip_id);
		ret = -ENODEV;
		goto out_unlock;
	}

	icmdev.spi = spi;

	ret = icm20608_hw_init(spi);
	if (ret) {
		goto err_sleep;
	}

	ret = alloc_chrdev_region(&icmdev.devid, 0, ICM20608_COUNT, ICM20608_NAME);
	if (ret) {
		goto err_sleep;
	}

	icmdev.class = class_create(THIS_MODULE, ICM20608_NAME);
	if (IS_ERR(icmdev.class)) {
		ret = PTR_ERR(icmdev.class);
		icmdev.class = NULL;
		goto err_region;
	}

	icmdev.cdev = cdev_alloc();
	if (!icmdev.cdev) {
		ret = -ENOMEM;
		goto err_class;
	}

	icmdev.cdev->owner = THIS_MODULE;
	icmdev.cdev->ops = &icm20608_fops;

	ret = cdev_add(icmdev.cdev, icmdev.devid, ICM20608_COUNT);
	if (ret) {
		goto err_put_cdev;
	}

	icmdev.device = device_create(
		icmdev.class, &spi->dev, icmdev.devid, NULL, ICM20608_NAME);
	if (IS_ERR(icmdev.device)) {
		ret = PTR_ERR(icmdev.device);
		icmdev.device = NULL;
		goto err_del_cdev;
	}

	spi_set_drvdata(spi, &icmdev);

	mutex_unlock(&icm20608_lock);

	dev_info(&spi->dev, "WHO_AM_I=0x%02x,/dev/icm20608 ready\n",
		(unsigned int)chip_id);

	return 0;

err_del_cdev:
	cdev_del(icmdev.cdev);
	icmdev.cdev = NULL;
	goto err_class;
err_put_cdev:
	kobject_put(&icmdev.cdev->kobj);
	icmdev.cdev = NULL;
err_class:
	class_destroy(icmdev.class);
err_region:
	unregister_chrdev_region(icmdev.devid, ICM20608_COUNT);
err_sleep:
	icm20608_sleep(spi);
	icmdev.spi = NULL;
out_unlock:
	mutex_unlock(&icm20608_lock);
	if (ret != -EPROBE_DEFER) {
		dev_err(&spi->dev, "probe failed: %d\n", ret);
	}
	return ret;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct icm20608_device *dev = spi_get_drvdata(spi);

	mutex_lock(&icm20608_lock);

	icm20608_sleep(spi);
	dev->spi = NULL;

	device_destroy(dev->class, dev->devid);
	dev->device = NULL;

	cdev_del(dev->cdev);
	dev->cdev = NULL;

	class_destroy(dev->class);
	dev->class = NULL;

	unregister_chrdev_region(dev->devid, ICM20608_COUNT);

	spi_set_drvdata(spi, NULL);

	mutex_unlock(&icm20608_lock);

	dev_info(&spi->dev, "removed\n");

	return 0;
}

static const struct spi_device_id icm20608_id[] = {
	{ "icm20608", 0 },
	{},
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "invensense,icm20608" },
	{},
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
	.probe = icm20608_probe,
	.remove = icm20608_remove,
	.id_table = icm20608_id,
	.driver = {
		.name = "icm20608_drv",
		.owner = THIS_MODULE,
		.of_match_table = icm20608_of_match,
	},
};

static int __init icm20608_init(void)
{
	return spi_register_driver(&icm20608_driver);
}

static void __exit icm20608_exit(void)
{
	spi_unregister_driver(&icm20608_driver);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("ICM20608 SPI character device driver");