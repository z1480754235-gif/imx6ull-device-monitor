#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/err.h>

#define KEYINPUT_NAME "keyinput"
#define KEYINPUT_PHYS "imx6ull-key/input0" /* 区分不同来源的输入设备 */

struct key_device {
	struct device *dev;
	struct gpio_desc *gpio;
	struct input_dev *input;
	struct delayed_work debounce_work;
	unsigned int code;
	unsigned int debounce_ms;
	int irq; /* 中断号 */
};

static void key_debounce_work(struct work_struct *work)
{
	struct key_device *key;
	int state;

	/* 已知的成员地址 要找的外层结构体类型 这个成员在外层结构体中的名字 */
	/* to_delayed_work(work)找到的是外层 delayed_work 的地址 */
	key = container_of(to_delayed_work(work), struct key_device, debounce_work);
	state = gpiod_get_value_cansleep(key->gpio);
	if (state < 0) {
		dev_err(key->dev, "failed to read key: %d\n", state);
		return;
	}

	/*
	 *GPIO_ACTIVE_LOW 已由 GPIO 框架处理
	 * 按下：物理低电平，逻辑值 1
	 * 松开：物理高电平，逻辑值 0
	*/
	input_report_key(key->input, key->code, state);
	input_sync(key->input);
}

static irqreturn_t key_irq_handler(int irq, void *dev_id)
{
	struct key_device *key = dev_id;

	/* 使用内核提供的工作队列 要安排的延迟工作 把毫秒转换成内核计时单位jiffies */
	mod_delayed_work(
		system_wq, &key->debounce_work, msecs_to_jiffies(key->debounce_ms));

	return IRQ_HANDLED;
}

static int key_probe(struct platform_device *pdev)
{
	struct key_device *key;
	int ret;

	key = devm_kzalloc(&pdev->dev, sizeof(struct key_device), GFP_KERNEL);
	if (!key) {
		return -ENOMEM;
	}

	key->dev = &pdev->dev;

	key->gpio = devm_gpiod_get(&pdev->dev, "key", GPIOD_IN);
	if (IS_ERR(key->gpio)) {
		ret = PTR_ERR(key->gpio);
		goto err_probe;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "linux,code", &key->code);
	if (ret) {
		goto err_probe;
	}
	if (key->code == KEY_RESERVED || key->code > KEY_MAX) {
		ret = -EINVAL;
		goto err_probe;
	}

	ret = of_property_read_u32(
		pdev->dev.of_node, "debounce-interval", &key->debounce_ms);
	if (ret) {
		goto err_probe;
	}
	if (key->debounce_ms == 0 || key->debounce_ms > 1000) {
		ret = -EINVAL;
		goto err_probe;
	}

	INIT_DELAYED_WORK(&key->debounce_work, key_debounce_work);

	/* 分配 input 设备 */
	key->input = devm_input_allocate_device(&pdev->dev);
	if (!key->input) {
		ret = -ENOMEM;
		goto err_probe;
	}

	key->input->name = KEYINPUT_NAME;
	key->input->phys = KEYINPUT_PHYS;
	key->input->id.bustype = BUS_HOST;

	/* 声明设备支持的按键事件 */
	input_set_capability(key->input, EV_KEY, key->code);

	ret = input_register_device(key->input);
	if (ret) {
		goto err_probe;
	}

	/* 将 GPIO 转换为 Linux 中断号 */
	key->irq = gpiod_to_irq(key->gpio);
	if (key->irq < 0) {
		ret = key->irq;
		goto err_probe;
	}

	/* 申请双边沿中断，检测按下和松开 */
	ret = devm_request_irq(&pdev->dev, key->irq, key_irq_handler,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, dev_name(&pdev->dev), key);

	if (ret) {
		goto err_cancel_work;
	}

	platform_set_drvdata(pdev, key);

	mod_delayed_work(
		system_wq, &key->debounce_work, msecs_to_jiffies(key->debounce_ms));

	dev_info(&pdev->dev, "key ready,irq = %d,code = %u\n", key->irq, key->code);

	return 0;

err_cancel_work:
	cancel_delayed_work_sync(&key->debounce_work);
err_probe:
	if (ret != -EPROBE_DEFER) {
		dev_err(&pdev->dev, "probe failed: %d\n", ret);
	}
	return ret;
}

static int key_remove(struct platform_device *pdev)
{
	struct key_device *key = platform_get_drvdata(pdev);

	/* 先停止中断，避免继续安排新的消抖工作 */
	devm_free_irq(&pdev->dev, key->irq, key);

	/* 等待正在执行的工作结束，并取消尚未执行的工作 */
	cancel_delayed_work_sync(&key->debounce_work);

	platform_set_drvdata(pdev, NULL);

	dev_info(&pdev->dev, "key removed\n");

	return 0;
}

static const struct of_device_id key_of_match[] = {
	{ .compatible = "smartmonitor,key0" },
	{},
};
MODULE_DEVICE_TABLE(of, key_of_match);

static struct platform_driver key_driver = {
	.probe = key_probe,
	.remove  = key_remove,
	.driver = {
		.name = "key_drv",
		.of_match_table = key_of_match,
	},
};

static int __init keydriver_init(void)
{
	return platform_driver_register(&key_driver);
}

static void __exit keydriver_exit(void)
{
	platform_driver_unregister(&key_driver);
}

module_init(keydriver_init);
module_exit(keydriver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("i.MX6ULL GPIO key input driver");