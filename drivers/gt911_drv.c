#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#define GT911_REG_INFO	 0x8140 /* 产品信息寄存器起始地址 */
#define GT911_REG_CONFIG 0x8047 /* 配置寄存器起始地址 */
#define GT911_REG_STATUS 0x814e /* 触摸数据状态寄存器 */
#define GT911_REG_POINTS 0x814f /* 坐标数据寄存器 */

#define GT911_MAX_TOUCH	 5
#define GT911_POINT_SIZE 8
#define GT911_DATA_READY 0x80

#define GT911_NAME "gt911"
#define GT911_PHYS "gt911/input0" /* 区分不同来源的输入设备 */

struct gt911_device {
	struct i2c_client *client;
	struct gpio_desc *reset;
	struct gpio_desc *irq_gpio;
	struct input_dev *input;

	unsigned char *point_data;

	unsigned short firmware;
	unsigned short x_size;
	unsigned short y_size;
	unsigned int max_touch;
	unsigned int trigger; /* 获取中断方式 */

	int slot_id[GT911_MAX_TOUCH]; /* 每个input槽位对应的Track ID，-1表示空闲 */
};

static unsigned short gt911_get_le16(const unsigned char *p)
{
	return ((unsigned short)p[1] << 8 | p[0]);
}

/* GT911 寄存器地址为 16 位，i2c_smbus_read_byte_data()函数不支持 */
/* 通信协议规定 寄存器地址先发送高字节，再发送低字节 */
/* GT911 支持从指定地址开始，连续读取多个寄存器的值 */
static int gt911_read_regs(struct i2c_client *client, unsigned short reg,
	unsigned char *buf, unsigned short len)
{
	unsigned char address[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr, /* 发给GT911 */
			.flags = 0, /* 写操作 */
			.len = sizeof(address), /* 发送2字节 */
			.buf = address, /* 发送内容 */
		},
		{
			.addr = client->addr, /* 再次发送从机地址 */
			.flags = I2C_M_RD, /* 读操作 */
			.len = len, /* 读取len字节 */
			.buf = buf, /* 结果存入buf */
		},
	};
	int ret;

	/* 返回的是完成的消息数量 */
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		return ret;
	}

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* 向寄存器写入数据 */
static int gt911_write_byte(struct i2c_client *client, unsigned short reg,
	unsigned char value)
{
	unsigned char data[3] = { reg >> 8, reg & 0xff, value };
	struct i2c_msg msg[1] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(data),
			.buf = data,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		return ret;
	}

	return ret == ARRAY_SIZE(msg) ? 0 : -EIO;
}

/*
 * 按GT911时序复位芯片，并选择7位I2C地址0x14。
 *
 * 时序：
 * 1. RESET输出低；
 * 2. INT输出高；选择0x14;输出低，选择0x5D
 * 3. INT状态保持100us以上；
 * 4. RESET输出高；
 * 5. 等待5ms以上；
 * 6. INT输出低并保持50ms以上；
 * 7. INT切换为浮空输入；
 * 8. 再等待50ms后访问GT911寄存器。
 */

/* 硬件复位，并按照 client->addr 选择 I2C 地址 */
static int gt911_reset_select_addr(struct gt911_device *ts)
{
	int ret;

	/* 复位 */
	gpiod_set_value_cansleep(ts->reset, 1);
	msleep(20);

	ret = gpiod_direction_output(ts->irq_gpio, ts->client->addr == 0x14);
	if (ret) {
		return ret;
	}

	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(ts->reset, 0);
	msleep(10);

	gpiod_set_value_cansleep(ts->irq_gpio, 0);
	msleep(60);

	ret = gpiod_direction_input(ts->irq_gpio);
	if (ret) {
		return ret;
	}

	msleep(50);

	return 0;
}

/* 读取芯片信息和配置 */
static int gt911_read_info(struct gt911_device *ts)
{
	struct i2c_client *client = ts->client;
	unsigned char info[10];
	unsigned char config[7];
	unsigned int expected;
	int ret;

	ret = gt911_read_regs(client, GT911_REG_INFO, info, sizeof(info));
	if (ret) {
		return ret;
	}

	if (memcmp(info, "911", 3) != 0) {
		dev_err(&client->dev, "unexpected product ID: %02x %02x %02x %02x\n",
			info[0], info[1], info[2], info[3]);
		return -ENODEV;
	}

	/* x_size、y_size保存的是xy轴的分辨率 */
	ts->firmware = gt911_get_le16(&info[4]);
	ts->x_size = gt911_get_le16(&info[6]);
	ts->y_size = gt911_get_le16(&info[8]);

	ret = gt911_read_regs(client, GT911_REG_CONFIG, config, sizeof(config));
	if (ret) {
		return ret;
	}

	ts->trigger = config[6] & 0x03;
	ts->max_touch = GT911_MAX_TOUCH;

	if (!ts->x_size || !ts->y_size) {
		return -EINVAL;
	}

	if (!of_property_read_u32(
			client->dev.of_node, "touchscreen-size-x", &expected) &&
		expected != ts->x_size) {
		dev_warn(&client->dev, "X size: chip=%u,DT=%u\n", ts->x_size, expected);
	}

	if (!of_property_read_u32(
			client->dev.of_node, "touchscreen-size-y", &expected) &&
		expected != ts->y_size) {
		dev_warn(&client->dev, "Y size: chip=%u,DT=%u\n", ts->y_size, expected);
	}

	return 0;
}

/* 释放全部触摸槽位 */
static void gt911_release_all(struct gt911_device *ts)
{
	unsigned int slot;

	for (slot = 0; slot < ts->max_touch; slot++) {
		/* 选中当前触摸槽位 */
		input_mt_slot(ts->input, slot);

		/* 上报当前槽位的手指已释放 */
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);

		ts->slot_id[slot] = -1;
	}

	/* 结束当前多点触摸帧并同步状态 */
	input_mt_sync_frame(ts->input);

	/* 上报本批输入事件的结束标记 */
	input_sync(ts->input);
}

/* 为Track ID查找或分配槽位 */
static int gt911_find_slot(struct gt911_device *ts, int id)
{
	unsigned int slot;

	/* 检查是否同一个手指 */
	for (slot = 0; slot < ts->max_touch; slot++) {
		if (ts->slot_id[slot] == id) {
			return slot;
		}
	}

	for (slot = 0; slot < ts->max_touch; slot++) {
		if (ts->slot_id[slot] < 0) {
			ts->slot_id[slot] = id;
			return slot;
		}
	}

	return -ENOSPC;
}

/* 读取、校验并上报一帧触摸数据 */
static int gt911_process_frame(struct gt911_device *ts)
{
	unsigned char status;
	unsigned char i;
	unsigned char *point;
	unsigned long seen = 0;
	unsigned int count;
	unsigned int slot;
	unsigned short x;
	unsigned short y;
	int id;
	int selected;
	int ret;
	int attempt;

	/* INT 可能略早于数据就绪 */
	for (attempt = 0; attempt < 20; attempt++) {
		ret = gt911_read_regs(ts->client, GT911_REG_STATUS, &status, 1);
		if (ret) {
			return ret;
		}
		if (status & GT911_DATA_READY) {
			break;
		}

		usleep_range(1000, 2000);
	}

	if (!(status & GT911_DATA_READY)) {
		return -EAGAIN;
	}

	/* 查看当前触摸点数 */
	count = status & 0x0f;
	if (count > ts->max_touch) {
		return -EPROTO;
	}
	if (count) {
		ret = gt911_read_regs(ts->client, GT911_REG_POINTS, ts->point_data,
			count * GT911_POINT_SIZE);
		if (ret) {
			return ret;
		}
	}

	/*
	 * 每个触点占 8 字节：
	 * 0：Track ID
	 * 1、2：X 低字节、高字节
	 * 3、4：Y 低字节、高字节
	 * 5、6：触点大小
	 * 7：保留
	 * 最多读取 5 条，即 0x814F～0x8176，共 40 字节。
	 */

	/* 先校验整帧，避免只更新部分触点 */
	for (i = 0; i < count; i++) {
		point = &ts->point_data[i * GT911_POINT_SIZE];

		id = point[0];
		x = gt911_get_le16(&point[1]);
		y = gt911_get_le16(&point[3]);

		/* BIT(n)，表示把第n位设为1，其余位为0 */
		if (id >= 16 || (seen & BIT(id)) || x >= ts->x_size ||
			y >= ts->y_size) {
			return -EPROTO;
		}

		/* 记录本帧出现的ID */
		seen |= BIT(id);
	}

	/* 释放本帧已消失的手指 */
	for (slot = 0; slot < ts->max_touch; slot++) {
		id = ts->slot_id[slot];

		if (id >= 0 && !(seen & BIT(id))) {
			input_mt_slot(ts->input, slot);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
			ts->slot_id[slot] = -1;
		}
	}

	/* 上报本帧的有效触点 */
	for (i = 0; i < count; i++) {
		point = &ts->point_data[i * GT911_POINT_SIZE];
		id = point[0];

		selected = gt911_find_slot(ts, id);
		if (selected < 0) {
			return selected;
		}

		input_mt_slot(ts->input, selected);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_abs(
			ts->input, ABS_MT_POSITION_X, gt911_get_le16(&point[1]));
		input_report_abs(
			ts->input, ABS_MT_POSITION_Y, gt911_get_le16(&point[3]));
	}

	/* 生成 BIN_TOUCH ABS_X ABS_Y 具体事件 */
	input_mt_sync_frame(ts->input);
	/* 生成SYN_REPORT */
	input_sync(ts->input);

	return 0;
}

/* 线程化中断 */
static irqreturn_t gt911_irq_thread(int irq, void *data)
{
	struct gt911_device *ts = data;
	int ret;

	ret = gt911_process_frame(ts);
	if (ret == -EAGAIN) {
		return IRQ_HANDLED;
	}
	if (ret) {
		dev_err_ratelimited(&ts->client->dev, "touch frame failed: %d\n", ret);
		gt911_release_all(ts);
	}

	ret = gt911_write_byte(ts->client, GT911_REG_STATUS, 0);
	if (ret) {
		dev_err_ratelimited(&ts->client->dev, "clear status failed: %d\n", ret);
		gt911_release_all(ts);
	}

	return IRQ_HANDLED;
}

static int gt911_input_init(struct gt911_device *ts)
{
	int ret;

	ts->input = devm_input_allocate_device(&ts->client->dev);
	if (!ts->input) {
		return -ENOMEM;
	}

	ts->input->name = GT911_NAME;
	ts->input->phys = GT911_PHYS;
	ts->input->id.bustype = BUS_I2C;
	ts->input->id.version = ts->firmware;

	/* 声明坐标的范围 声明事件EV_ABS */
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->x_size - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->y_size - 1, 0, 0);

	/* 准备了5个槽位 声明为直接触摸设备 未更新的活动槽位自动释放 */
	/* 声明 ABS_MT_SLOT、ABS_MT_TRACKING_ID */
	/* INPUT_MT_DIRECT 声明 EV_KEY 和 BTN_TOUCH 补充 ABS_X 和 ABS_Y */
	ret = input_mt_init_slots(
		ts->input, ts->max_touch, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		return ret;
	}

	/* 声明 EV_SYN */
	return input_register_device(ts->input);
}

static int gt911_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	static const unsigned long irq_flags[] = {
		IRQF_TRIGGER_RISING,
		IRQF_TRIGGER_FALLING,
		IRQF_TRIGGER_LOW,
		IRQF_TRIGGER_HIGH,
	};
	static const char *const trigger_names[] = {
		"rising edge",
		"falling edge",
		"low level",
		"high level",
	};
	struct gt911_device *ts;
	unsigned char i;
	int ret;

	/* I²C 适配器是否支持普通 I²C 传输 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		ret = -EOPNOTSUPP;
		goto err_probe;
	}

	if (client->addr != 0x14 && client->addr != 0x5d) {
		ret = -EINVAL;
		goto err_probe;
	}

	ts = devm_kzalloc(&client->dev, sizeof(struct gt911_device), GFP_KERNEL);
	if (!ts) {
		ret = -ENOMEM;
		goto err_probe;
	}

	ts->client = client;

	for (i = 0; i < GT911_MAX_TOUCH; i++) {
		ts->slot_id[i] = -1;
	}

	/* 复位引脚低电平有效 */
	ts->reset = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset)) {
		ret = PTR_ERR(ts->reset);
		goto err_probe;
	}

	ts->irq_gpio = devm_gpiod_get(&client->dev, "irq", GPIOD_OUT_LOW);
	if (IS_ERR(ts->irq_gpio)) {
		ret = PTR_ERR(ts->irq_gpio);
		goto err_reset;
	}

	ret = gt911_reset_select_addr(ts);
	if (ret) {
		goto err_reset;
	}

	ret = gt911_read_info(ts);
	if (ret) {
		goto err_reset;
	}

	ts->point_data = devm_kzalloc(
		&client->dev, ts->max_touch * GT911_POINT_SIZE, GFP_KERNEL);
	if (!ts->point_data) {
		ret = -ENOMEM;
		goto err_reset;
	}

	ret = gt911_input_init(ts);
	if (ret) {
		goto err_reset;
	}

	/* 清除可能残留的就绪状态 */
	ret = gt911_write_byte(client, GT911_REG_STATUS, 0);
	if (ret) {
		goto err_reset;
	}

	i2c_set_clientdata(client, ts);

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
		gt911_irq_thread, irq_flags[ts->trigger] | IRQF_ONESHOT,
		dev_name(&client->dev), ts);
	if (ret) {
		goto err_reset;
	}

	dev_info(&client->dev,
		"GT911 detected: addr = 0x%02x,irq = %d,firmware = 0x%04x\n",
		client->addr, client->irq, ts->firmware);

	dev_info(&client->dev, "resolution = %ux%u,max_touch = %u,trigger = %s\n",
		ts->x_size, ts->y_size, ts->max_touch, trigger_names[ts->trigger]);

	return 0;

err_reset:
	gpiod_set_value_cansleep(ts->reset, 1);
	i2c_set_clientdata(client, NULL);
err_probe:
	if (ret != -EPROBE_DEFER) {
		dev_err(&client->dev, " probe failed: %d\n", ret);
	}

	return ret;
}

static int gt911_remove(struct i2c_client *client)
{
	struct gt911_device *ts = i2c_get_clientdata(client);

	devm_free_irq(&client->dev, client->irq, ts);

	gt911_release_all(ts);

	gpiod_set_value_cansleep(ts->reset, 1);

	i2c_set_clientdata(client, NULL);

	dev_info(&client->dev, "removed\n");

	return 0;
}

static const struct i2c_device_id gt911_id[] = {
	{ "gt911", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, gt911_id);

static const struct of_device_id gt911_of_match[] = {
	{ .compatible = "goodix,gt911" },
	{},
};
MODULE_DEVICE_TABLE(of, gt911_of_match);

static struct i2c_driver gt911_driver = {
	.probe = gt911_probe,
	.remove = gt911_remove,
	.id_table = gt911_id,
	.driver = {
		.name = "gt911_drv",
		.of_match_table = gt911_of_match,
	},
};

static int __init gt911_init(void)
{
	return i2c_add_driver(&gt911_driver);
}

static void __exit gt911_exit(void)
{
	i2c_del_driver(&gt911_driver);
}

module_init(gt911_init);
module_exit(gt911_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zero");
MODULE_DESCRIPTION("GT911 reset and identification test driver");