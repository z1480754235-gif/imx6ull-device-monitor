# BSP 内核源码目录
KERNEL_DIR ?= /home/zero/linux/IMX6ULL/linux/original_linux

#交叉编译工具前缀
CROSS_COMPILE ?= arm-linux-gnueabihf-

#使用BSP内核中的设备树编译器
DTC := $(KERNEL_DIR)/scripts/dtc/dtc

#输出目录和设备树名称
BUILD_DIR := build
DT_NAME := imx6ull-device-management

.PHONY: all dtb modules app clean

all: dtb modules app

#GCC 的 -I 表示搜索目录，DTC 的 -I 表示输入格式
dtb: | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc -E -nostdinc -undef -D__DTS__ \
		-x assembler-with-cpp \
		-I$(KERNEL_DIR)/arch/arm/boot/dts \
		-I$(KERNEL_DIR)/include \
		-Idts \
		-o $(BUILD_DIR)/$(DT_NAME).dts \
		dts/$(DT_NAME).dts
	$(DTC) -I dts -O dtb \
		-o $(BUILD_DIR)/$(DT_NAME).dtb \
		$(BUILD_DIR)/$(DT_NAME).dts

$(BUILD_DIR):
	mkdir -p $@

modules:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/drivers" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules

app: $(BUILD_DIR)/led_test $(BUILD_DIR)/key_test \
	$(BUILD_DIR)/key_led_test $(BUILD_DIR)/ap3216c_test \
	$(BUILD_DIR)/icm20608_test $(BUILD_DIR)/uart_test \
	$(BUILD_DIR)/touch_test

$(BUILD_DIR)/led_test: app/led_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/key_test: app/key_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/key_led_test: app/key_led_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/ap3216c_test: app/ap3216c_test.c include/ap3216c_data.h | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/icm20608_test: app/icm20608_test.c include/icm20608_data.h | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/uart_test: app/uart_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/touch_test: app/touch_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

clean:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/drivers" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -rf $(BUILD_DIR)
