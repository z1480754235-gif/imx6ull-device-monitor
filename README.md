# i.MX6ULL 嵌入式设备监测与管理系统

基于正点原子 i.MX6ULL ALPHA 开发板的嵌入式 Linux 项目。通过自定义设备树和 Linux 驱动采集板载传感器数据，在 1024×600 触摸屏上使用 Qt Widgets 显示，并实现 LED 手动／自动控制、参数保存和 CSV 数据记录。

项目包含设备驱动、C 测试程序和 Qt 应用源码；BSP 内核、交叉工具链、Qt 运行库和字体需单独准备。

## 第一版功能

| 模块 | 已实现内容 |
| --- | --- |
| LED | 字符设备控制、触摸按钮和实体按键控制 |
| 实体按键 | GPIO 中断、延迟工作消抖、Input 事件上报 |
| AP3216C | I²C 读取红外、环境光和接近数据，保留有效标志 |
| ICM20608 | SPI 读取三轴加速度、三轴角速度及芯片温度 |
| GT911 | I²C 通信、线程化中断、最多 5 个触点的多点触摸上报 |
| UART3 | 使用 BSP 串口驱动进行 115200、8N1 收发回环测试 |
| Qt 界面 | Monitor / Settings 双页，数据刷新、设备状态和错误提示 |
| 自动控制 | ALS 双阈值控制 LED，支持手动／自动切换 |
| 参数设置 | 修改光照阈值和采集间隔，使用 QSettings 保存、恢复 |
| 数据记录 | 两路传感器分别保存带时间戳的 CSV，支持开始／停止记录 |

UART 使用内核已有的 i.MX 串口驱动，本项目提供用户态回环测试程序。

## 硬件与软件环境

| 项目 | 开发环境 |
| --- | --- |
| 开发板 | 正点原子 i.MX6ULL ALPHA，eMMC 版本 |
| CPU | ARM Cortex-A7，32 位，小端 |
| Linux | BSP 内核 4.1.15 |
| 工具链 | Linaro GCC 4.9.4，`arm-linux-gnueabihf-` |
| Qt | Qt 5.12.9，Widgets，C++11 |
| 显示屏 | 1024×600 LCD，GT911 电容触摸 |
| 显示接口 | `/dev/fb0`，当前使用 32 bpp framebuffer |
| 调试部署 | 串口终端、TFTP 内核／设备树、NFS 根文件系统 |

驱动接口按 Linux 4.1.15 编写，移植到其他内核版本时可能需要调整相关 API。

## 目录结构

```text
.
├── Makefile                       # 设备树、驱动模块和 C 应用构建入口
├── app/                           # 单外设和联动测试程序
├── drivers/                       # LED、按键、AP3216C、ICM20608、GT911 驱动
│   └── Makefile                   # 内核模块 Kbuild 配置
├── dts/
│   ├── imx6ull-device-management.dts   # 项目设备树编译入口
│   └── imx6ull-device-management.dtsi  # 项目外设配置
├── include/                       # 驱动与应用共享的数据结构、换算系数
└── qt/device_monitor/
    ├── device_monitor.pro         # qmake 工程
    ├── main.cpp                   # 应用入口
    ├── mainwindow.h/.cpp          # 界面、控制、配置、CSV 记录
    ├── mainwindow.ui              # Designer 界面
    └── sensorworker.h/.cpp        # 工作线程中的传感器采集
```

`build/`、内核模块编译产物、Qt Creator 的本机配置、运行配置和采集文件不提交到仓库。

## 引脚与设备节点

以下以当前开发板原理图及项目设备树为准：

| 设备 | 接口／引脚 | 用户态入口 |
| --- | --- | --- |
| LED | GPIO1_IO03，低电平有效 | `/dev/led0` |
| 按键 | GPIO1_IO18，低电平有效，`KEY_ENTER` | `/dev/input/eventX` |
| AP3216C | I2C1，7 位地址 `0x1e` | `/dev/ap3216c` |
| ICM20608 | ECSPI3，GPIO1_IO20 软件片选，SPI Mode 0 | `/dev/icm20608` |
| UART3 | UART3_TX_DATA / UART3_RX_DATA | `/dev/ttymxc2` |
| GT911 | I2C2，7 位地址 `0x14`；INT：GPIO1_IO09；RST：GPIO5_IO09，低电平复位 | `/dev/input/eventX` |
| LCD | BSP 已配置的 LCD 控制器及背光 | `/dev/fb0` |

Input 的 `eventX` 编号不是固定值。运行时使用以下命令确定按键和触摸屏对应的节点：

```sh
cat /proc/bus/input/devices
```

Qt 当前按键路径写在 `mainwindow.cpp`，默认为 `/dev/input/event2`。触摸路径由 Qt 输入环境配置，部署时需要按实际节点调整。

## BSP 与设备树依赖

项目入口包含 BSP 的 `imx6ull-alientek-emmc.dts`，再包含项目 `.dtsi`。原始板级设备树及 SoC 公共描述仍由 BSP 管理，本仓库不包含完整内核。

编译前应确认：

- BSP 的 `arch/arm/boot/dts/` 下存在板级 `.dts` 及其包含文件。
- BSP 中保留已调试好的 LCD 时序、背光和 I²C 控制器配置。
- BSP 旧 LED、按键、AP3216C、ICM20608 和触摸节点不再与项目节点重复定义或占用相同资源。
- 项目使用了 BSP 的 `pinctrl_hog_1`、`reg_sd1_vmmc` 等标签，需要和所用 BSP 对应。
- GT911 使用的 GPIO5_IO09 不再由旧 LCD reset 配置占用；GPIO1_IO09 由项目配置释放给触摸中断。

当前设备树针对本项目使用的 BSP 已做适配，不是对任意原版 NXP 设备树直接通用的覆盖文件。

## 编译

### 1. 驱动、设备树和 C 测试程序

先将交叉编译器的 `bin` 目录加入 `PATH`，并准备好与开发板运行内核一致、已配置和编译的 BSP 内核树。内核树中需要有可运行的 `scripts/dtc/dtc`。

在项目根目录执行：

```sh
make KERNEL_DIR=/path/to/bsp/linux CROSS_COMPILE=arm-linux-gnueabihf-
```

也可以分别执行：

```sh
make dtb KERNEL_DIR=/path/to/bsp/linux
make modules KERNEL_DIR=/path/to/bsp/linux
make app
```

产物位置：

- 设备树：`build/imx6ull-device-management.dtb`。
- 内核模块：`drivers/*.ko`。
- C 测试程序：`build/led_test`、`key_test`、`key_led_test`、`ap3216c_test`、`icm20608_test`、`uart_test`、`touch_test`。

`app/touch.c` 是保留的分步调试示例，当前顶层 `make app` 不编译它。

清理命令：

```sh
make clean KERNEL_DIR=/path/to/bsp/linux
```

`clean` 会清理驱动构建产物，并删除整个项目 `build/` 目录，包括放在其中的 Qt 构建目录。

### 2. Qt 应用

Qt 应用独立编译，顶层 `make` 不包含 Qt 工程。使用为 ARM 交叉编译好的 Qt 5.12.9 的 `qmake`：

```sh
mkdir -p build/qt-device-monitor
cd build/qt-device-monitor
/path/to/arm-qt/bin/qmake ../../qt/device_monitor/device_monitor.pro
make -j2
```

生成 `device_monitor`。也可在 Qt Creator 中打开 `.pro`，选择 ARM Qt Kit 构建。

不要使用桌面 GCC 64 位 Kit 构建要部署到开发板的程序。开发板还需安装匹配的 ARM Qt 运行库和 `linuxfb` 平台插件。

## 部署与运行

### 1. 更新设备树

将生成的 DTB 放入 TFTP 目录，并让 U-Boot 加载该项目 DTB 后启动内核。内核模块必须与开发板正在运行的内核匹配。

本项目开发环境使用：

```text
TFTP：/home/zero/linux/tftpboot
NFS 根目录：/home/zero/linux/nfs/rootfs
主机部署目录：/home/zero/linux/nfs/rootfs/device-management
开发板对应目录：/device-management
```

这些路径属于部署环境，其他机器请按自己的服务配置调整。

### 2. 加载驱动

把 `.ko`、测试程序及 Qt 可执行文件复制到部署目录。在开发板上执行：

```sh
cd /device-management
insmod led_drv.ko
insmod key_drv.ko
insmod ap3216c_drv.ko
insmod icm20608_drv.ko
insmod gt911_drv.ko
dmesg | tail -n 30
cat /proc/bus/input/devices
```

第一次部署时先确认驱动成功绑定、设备节点存在，再启动 Qt。各模块不要求按以上顺序加载，但 Input 节点编号可能随加载顺序改变。

### 3. 准备 Qt 运行环境

确保动态链接器能够找到 Qt 和 tslib 的库，并设置平台插件路径。项目调试环境的 Qt、tslib 分别放在 `/usr/lib/arm-qt`、`/usr/lib/arm-tslib`，字体放在 `/usr/share/fonts`。

如果这些环境变量已经写入开发板 `/etc/profile`，重新加载后启动：

```sh
. /etc/profile
cd /device-management
./device_monitor -platform linuxfb:fb=/dev/fb0
```

使用 tslib 时，`TSLIB_TSDEVICE` 应指向 GT911 的实际事件节点，并设置 `QT_QPA_FB_TSLIB=1`。`QT_QPA_FONTDIR` 应指向已准备好的字体目录；界面指定字体为 Consolas，字体需自行准备，不随源码分发。

显示未亮时，可检查 framebuffer 消隐、背光电源和亮度：

```sh
echo 0 > /sys/class/graphics/fb0/blank
echo 0 > /sys/class/backlight/backlight/bl_power
echo 6 > /sys/class/backlight/backlight/brightness
```

背光节点名称及有效亮度范围以实际 BSP 为准。

## 操作说明

### Monitor 与设备控制

- Environment 显示 AP3216C 数据；`Read Sensor` 额外请求采集一次 AP3216C。
- Motion 显示 ICM20608 换算后的加速度、角速度和芯片温度。
- `OK` 表示本次设备数据读取成功；`ERROR` 表示打开或读取失败。AP 的各通道还会单独显示 `invalid`。
- 手动模式下，屏幕 LED 按钮或实体按键可以翻转 LED。
- 勾选 `Auto LED` 后，手动 LED 按钮禁用，实体按键只显示按下／松开状态。
- 自动模式使用 ALS 原始数值判断：小于等于开灯阈值时开灯，大于等于关灯阈值时关灯，中间保持当前状态。
- ALS 无效或读取失败时不进行自动判断，保持 LED 当前状态。

### Settings

| 参数 | 默认值 | 范围 |
| --- | --- | --- |
| LED ON threshold | 20 | 0～65535 |
| LED OFF threshold | 50 | 0～65535 |
| Sample interval | 1000 ms | 200～60000 ms |

开灯阈值必须小于关灯阈值。修改后点击 `Apply` 才生效，两个传感器使用同一采集间隔。ALS 原始值不是 lux；温度为 ICM20608 芯片温度，不应直接当作环境温度。

参数保存到程序同目录的 `device_monitor.ini`，下次启动恢复。文件内容示例：

```ini
[led]
onThreshold=20
offThreshold=50

[sensor]
intervalMs=1000
```

每次启动仍为手动模式。配置不合理时使用默认值；保存失败时，本次应用的参数仍生效，但不保证下次恢复。

### CSV 数据记录

点击 `Start Recording` 开始，点击 `Stop Recording` 停止。每次记录创建两个新文件：

```text
records/ap3216c_年月日_时分秒_毫秒.csv
records/icm20608_年月日_时分秒_毫秒.csv
```

目录位于可执行程序旁。AP 文件保存原始值和有效标志，无效通道留空；ICM 文件保存换算后的 `g`、`dps` 和 `C`。

每收到一次成功的采集结果追加一行，时间戳使用界面处理该结果时的开发板系统时间。默认每路约 1 秒一行；手动读取 AP 会额外追加一行。传感器失败期间没有对应数据行。记录前应先确认开发板系统时间。

点击 `Exit` 正常退出时，会结束工作线程、关闭记录文件、关灯并关闭设备。当前没有安装 Linux 退出信号处理，默认 `Ctrl+C` 不执行 C++ 析构清理。

## 独立测试示例

以下事件节点仅为示例，先根据 `/proc/bus/input/devices` 确认实际编号：

```sh
./led_test /dev/led0 1
./led_test /dev/led0 0
./key_test /dev/input/event2
./key_led_test /dev/input/event2 /dev/led0
./ap3216c_test /dev/ap3216c
./icm20608_test /dev/icm20608
./touch_test /dev/input/event1
./uart_test /dev/ttymxc2
```

LED 接口使用一个字节的 ASCII 字符 `'0'` / `'1'`。UART 回环测试需短接 UART3 的 TX 与 RX；它验证本机串口收发，不包含外部传感器协议。

## 程序执行结构

```text
Linux 驱动 / Input 事件
          │
          ├── SensorWorker（工作线程）
          │      ├── QTimer 定时读取 AP3216C、ICM20608
          │      └── 信号传回采集结果与错误
          │
          └── MainWindow（界面线程）
                 ├── 更新监测数据与状态
                 ├── QSocketNotifier 读取实体按键
                 ├── 手动 / 自动 LED 控制
                 ├── QSettings 保存参数
                 └── 小规模 CSV 文件记录
```

显示与触摸分别通过 framebuffer 和 Input / tslib 接入。界面线程不直接等待传感器采集，运行时的定时器间隔修改通过队列信号交给工作线程。

## 常见问题

- **模块加载但没有设备节点**：查看 `dmesg`，检查设备树是否实际加载、`compatible` 是否匹配及 `probe()` 是否成功。
- **提示 GPIO 被占用**：检查 BSP 是否仍启用了同引脚的旧设备或 pinctrl 配置。
- **按键或触摸没有响应**：重新确认 Input 节点；Qt 按键路径及 tslib 触摸路径可能需要调整。
- **Qt 缺少平台插件或动态库**：确认部署的是 ARM 库及 `linuxfb` 插件，并检查运行环境路径。
- **配置或记录保存失败**：确认程序目录可写、存储空间足够。程序不会自动清理旧 CSV 文件。
- **设备在程序启动后才加载驱动**：当前传感器打开失败不会自动重新打开，加载驱动后重启 Qt 程序。

## 后续计划

第一版完成数据采集、显示、控制、配置和记录。后续在保留当前可用版本的基础上增加蜂鸣器等板载外设驱动。
