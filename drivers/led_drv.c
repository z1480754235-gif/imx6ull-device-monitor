#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define LED_NAME  "led0"
#define LED_COUNT 1

struct led_device {
	dev_t devid;
	int major;
	int minor;
	struct cdev *cdev;
	struct class *class;
	struct device *device;
	struct gpio_desc *led; /* GPIO描述符 */
};

static struct led_device leddev;

/* 保护 GPIO 访问，防止写操作与设备解绑同时发生 */
static DEFINE_MUTEX(led_lock);

static int led_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	mutex_lock(&led_lock);

	if (!leddev.led || inode->i_cdev != leddev.cdev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	filp->private_data = &leddev;

out_unlock:
	mutex_unlock(&led_lock);
	return ret;
}

static ssize_t led_write(struct file *filp, const char __user *buf, size_t cnt,
	loff_t *offt)
{
	struct led_device *dev = filp->private_data;
	char data[1];
	int value;
	int ret;

	if (cnt == 0) {
		return -EINVAL;
	}

	if (cnt != sizeof(data)) {
		return -EINVAL;
	}

	ret = copy_from_user(data, buf, cnt);
	if (ret) {
		return -EFAULT;
	}

	switch (data[0]) {
	case '1':
		value = 1;
		break;
	case '0':
		value = 0;
		break;
	default:
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&led_lock);
	if (ret) {
		return ret;
	}

	if (!dev->led || file_inode(filp)->i_cdev != dev->cdev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	gpiod_set_value_cansleep(dev->led, value);

	mutex_unlock(&led_lock);

	return cnt;

out_unlock:
	mutex_unlock(&led_lock);
	return ret;
}

static const struct file_operations led_fops = {
	.owner = THIS_MODULE,
	.open = led_open,
	.write = led_write,
	.llseek = no_llseek, /* 这个设备不支持通过 lseek() 调整文件读写位置 */
};

static int led_probe(struct platform_device *pdev)
{
	int ret;

	mutex_lock(&led_lock);

	if (leddev.led) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/* 通过 "led" 查找 led-gpios */
	leddev.led = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(leddev.led)) {
		ret = PTR_ERR(leddev.led);
		leddev.led = NULL;
		goto out_unlock;
	}

	ret = alloc_chrdev_region(&leddev.devid, 0, LED_COUNT, LED_NAME);
	if (ret) {
		goto err_gpio;
	}

	leddev.major = MAJOR(leddev.devid);
	leddev.minor = MINOR(leddev.devid);
	dev_info(&pdev->dev, "major = %d,minor = %d\n", leddev.major, leddev.minor);

	leddev.class = class_create(THIS_MODULE, LED_NAME);
	if (IS_ERR(leddev.class)) {
		ret = PTR_ERR(leddev.class);
		leddev.class = NULL;
		goto err_region;
	}

	/* 动态分配并初始化 cdev */
	leddev.cdev = cdev_alloc();
	if (!leddev.cdev) {
		ret = -ENOMEM;
		goto err_class;
	}

	leddev.cdev->owner = THIS_MODULE;
	leddev.cdev->ops = &led_fops;

	gpiod_set_value_cansleep(leddev.led, 0);

	ret = cdev_add(leddev.cdev, leddev.devid, LED_COUNT);
	if (ret) {
		goto err_put_cdev;
	}

	leddev.device =
		device_create(leddev.class, &pdev->dev, leddev.devid, NULL, LED_NAME);
	if (IS_ERR(leddev.device)) {
		ret = PTR_ERR(leddev.device);
		leddev.device = NULL;
		goto err_del_cdev;
	}

	platform_set_drvdata(pdev, &leddev);

	mutex_unlock(&led_lock);

	/* 输出一般信息日志 */
	dev_info(&pdev->dev, "/dev/led0 ready,LED OFF\n");

	return 0;

err_del_cdev:
	cdev_del(leddev.cdev);
	leddev.cdev = NULL;
	goto err_class;
err_put_cdev:
	kobject_put(&leddev.cdev->kobj);
	leddev.cdev = NULL;
err_class:
	class_destroy(leddev.class);
	leddev.class = NULL;
err_region:
	unregister_chrdev_region(leddev.devid, LED_COUNT);
err_gpio:
	gpiod_set_value_cansleep(leddev.led, 0);
	leddev.led = NULL;
	/* GPIO 申请资源由 devm 在 probe 失败后释放 */
out_unlock:
	mutex_unlock(&led_lock);
	if (ret != -EPROBE_DEFER) {
		dev_err(&pdev->dev, "probe failed: %d\n", ret);
	}
	return ret;
}

static int led_remove(struct platform_device *pdev)
{
	struct led_device *dev = platform_get_drvdata(pdev);

	mutex_lock(&led_lock);

	gpiod_set_value_cansleep(dev->led, 0);
	dev->led = NULL;

	device_destroy(dev->class, dev->devid);
	dev->device = NULL;

	cdev_del(dev->cdev);
	dev->cdev = NULL;

	class_destroy(dev->class);
	dev->class = NULL;

	unregister_chrdev_region(dev->devid, LED_COUNT);

	platform_set_drvdata(pdev, NULL);

	mutex_unlock(&led_lock);

	/* GPIO 申请资源由 devm 在 remove 返回后释放 */
	dev_info(&pdev->dev, "remove, LED OFF\n");

	return 0;
}

static const struct of_device_id led_of_match[] = {
	{ .compatible = "smartmonitor,led0" },
	{},
};

static struct platform_driver led_driver = {
	.probe = led_probe,
	.remove = led_remove,
	.driver = {
		.name = "led_drv",
		.of_match_table = led_of_match,
	},
};

static int __init leddriver_init(void)
{
	return platform_driver_register(&led_driver);
}

static void __exit leddriver_exit(void)
{
	platform_driver_unregister(&led_driver);
}

module_init(leddriver_init);
module_exit(leddriver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("i.MX6ULL single LED character device driver");