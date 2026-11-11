#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "../include/ap3216c_data.h"

#define AP3216C_NAME  "ap3216c"
#define AP3216C_COUNT 1

#define AP3216C_SYSTEM_CONFIG 0x00
#define AP3216C_IR_DATA_LOW	  0x0a

#define AP3216C_MODE_OFF   0x00
#define AP3216C_MODE_ALL   0x03
#define AP3216C_MODE_RESET 0x04

struct ap3216c_device {
	struct i2c_client *client;
	dev_t devid;
	int major;
	int minor;
	struct cdev *cdev;
	struct class *class;
	struct device *device;
};

static struct ap3216c_device apdev;
static DEFINE_MUTEX(ap3216c_lock);

/* 关闭测量 */
static void ap3216c_power_off(struct i2c_client *client)
{
	int ret;

	ret = i2c_smbus_write_byte_data(
		client, AP3216C_SYSTEM_CONFIG, AP3216C_MODE_OFF);
	if (ret < 0) {
		dev_warn(&client->dev, "power off failed: %d\n", ret);
	}
}

/* AP3216C复位并启用三路测量 */
static int ap3216c_hw_init(struct i2c_client *client)
{
	int ret;

	ret = i2c_smbus_write_byte_data(
		client, AP3216C_SYSTEM_CONFIG, AP3216C_MODE_RESET);
	if (ret < 0) {
		return ret;
	}

	msleep(50);

	ret = i2c_smbus_write_byte_data(
		client, AP3216C_SYSTEM_CONFIG, AP3216C_MODE_ALL);
	if (ret < 0) {
		return ret;
	}

	msleep(200);

	ret = i2c_smbus_read_byte_data(client, AP3216C_SYSTEM_CONFIG);
	if (ret < 0) {
		return ret;
	}

	if (ret != AP3216C_MODE_ALL) {
		return -EIO;
	}

	return 0;
}

static int ap3216c_read_raw(struct i2c_client *client,
	struct ap3216c_data *data)
{
	unsigned char buf[6];
	unsigned char i;
	int ret;

	for (i = 0; i < 6; i++) {
		ret = i2c_smbus_read_byte_data(client, AP3216C_IR_DATA_LOW + i);
		if (ret < 0) {
			return ret;
		}

		buf[i] = ret;
	}

	/* 红外强度原始值 环境光原始值 接近检测原始值 */
	data->ir = ((unsigned short)buf[1] << 2) | (buf[0] & 0x03);
	data->als = ((unsigned short)buf[3] << 8) | buf[2];
	data->ps = ((unsigned short)(buf[5] & 0x3f) << 4) | (buf[4] & 0x0f);

	/* 检查红外溢出标志，标记数据是否有效 */
	data->flags = AP3216C_ALS_VALID;

	if (!(buf[0] & 0x80)) {
		data->flags |= AP3216C_IR_VALID;
	}
	if (!(buf[0] & 0x80) && !((buf[4] | buf[5]) & 0x40)) {
		data->flags |= AP3216C_PS_VALID;
	}

	return 0;
}

static int ap3216c_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	mutex_lock(&ap3216c_lock);

	if (!apdev.client || inode->i_cdev != apdev.cdev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	filp->private_data = &apdev;

out_unlock:
	mutex_unlock(&ap3216c_lock);
	return ret;
}

static ssize_t ap3216c_read(struct file *filp, char __user *buf, size_t cnt,
	loff_t *offt)
{
	struct ap3216c_device *dev = filp->private_data;
	struct ap3216c_data data;
	int ret;

	if (cnt == 0) {
		return 0;
	}

	if (cnt < sizeof(data)) {
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&ap3216c_lock);
	if (ret) {
		return ret;
	}

	if (!dev->client || file_inode(filp)->i_cdev != dev->cdev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = ap3216c_read_raw(dev->client, &data);
	if (ret) {
		goto out_unlock;
	}

	mutex_unlock(&ap3216c_lock);

	if (copy_to_user(buf, &data, sizeof(data))) {
		return -EFAULT;
	}

	return sizeof(data);

out_unlock:
	mutex_unlock(&ap3216c_lock);
	return ret;
}

static const struct file_operations ap3216c_fops = {
	.owner = THIS_MODULE,
	.open = ap3216c_open,
	.read = ap3216c_read,
	.llseek = no_llseek,
};

static int ap3216c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret;

	mutex_lock(&ap3216c_lock);

	/* 限制当前驱动只管理一个 AP3216C，避免覆盖已有设备的数据。 */
	if (apdev.client) {
		ret = -EBUSY;
		goto out_unlock;
	}
	/* 检查控制器是否支持所需的寄存器访问操作 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	apdev.client = client;

	ret = ap3216c_hw_init(client);
	if (ret) {
		goto err_power_off;
	}

	ret = alloc_chrdev_region(&apdev.devid, 0, AP3216C_COUNT, AP3216C_NAME);
	if (ret) {
		goto err_power_off;
	}

	apdev.major = MAJOR(apdev.devid);
	apdev.minor = MINOR(apdev.devid);
	dev_info(&client->dev, "major = %d,minor = %d\n", apdev.major, apdev.minor);

	apdev.class = class_create(THIS_MODULE, AP3216C_NAME);
	if (IS_ERR(apdev.class)) {
		ret = PTR_ERR(apdev.class);
		apdev.class = NULL;
		goto err_region;
	}

	apdev.cdev = cdev_alloc();
	if (!apdev.cdev) {
		ret = -ENOMEM;
		goto err_class;
	}

	apdev.cdev->owner = THIS_MODULE;
	apdev.cdev->ops = &ap3216c_fops;

	ret = cdev_add(apdev.cdev, apdev.devid, AP3216C_COUNT);
	if (ret) {
		goto err_put_cdev;
	}

	apdev.device = device_create(
		apdev.class, &client->dev, apdev.devid, NULL, AP3216C_NAME);
	if (IS_ERR(apdev.device)) {
		ret = PTR_ERR(apdev.device);
		apdev.device = NULL;
		goto err_del_cdev;
	}

	/* 设备数据 apdev 的地址，保存到这个 I²C 设备 client中 */
	i2c_set_clientdata(client, &apdev);

	mutex_unlock(&ap3216c_lock);

	dev_info(&client->dev, "/dev/ap3216c ready\n");

	return 0;

err_del_cdev:
	cdev_del(apdev.cdev);
	apdev.cdev = NULL;
	goto err_class;
err_put_cdev:
	kobject_put(&apdev.cdev->kobj);
	apdev.cdev = NULL;
err_class:
	class_destroy(apdev.class);
	apdev.class = NULL;
err_region:
	unregister_chrdev_region(apdev.devid, AP3216C_COUNT);
err_power_off:
	ap3216c_power_off(client);
	apdev.client = NULL;
out_unlock:
	mutex_unlock(&ap3216c_lock);
	if (ret != -EPROBE_DEFER) {
		dev_err(&client->dev, "probe failed: %d\n", ret);
	}
	return ret;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct ap3216c_device *dev = i2c_get_clientdata(client);

	mutex_lock(&ap3216c_lock);

	ap3216c_power_off(client);
	dev->client = NULL;

	device_destroy(dev->class, dev->devid);
	dev->device = NULL;

	cdev_del(dev->cdev);
	dev->cdev = NULL;

	class_destroy(dev->class);
	dev->class = NULL;

	unregister_chrdev_region(dev->devid, AP3216C_COUNT);

	i2c_set_clientdata(client, NULL);

	mutex_unlock(&ap3216c_lock);

	dev_info(&client->dev, "removed\n");

	return 0;
}

static const struct i2c_device_id ap3216c_id[] = {
	{ "ap3216c", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "liteon,ap3216c" },
	{},
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_driver = {
	.probe = ap3216c_probe,
	.remove = ap3216c_remove,
	.id_table = ap3216c_id,
	.driver = {
		.name = "ap3216c_drv",
		.of_match_table = ap3216c_of_match,
	},
};

static int __init ap3216c_init(void)
{
	return i2c_add_driver(&ap3216c_driver);
}

static void __exit ap3216c_exit(void)
{
	i2c_del_driver(&ap3216c_driver);
}

module_init(ap3216c_init);
module_exit(ap3216c_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("AP3216C raw data character device driver");