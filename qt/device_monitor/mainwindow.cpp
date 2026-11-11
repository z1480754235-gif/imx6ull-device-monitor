#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ap3216c_data.h"
#include "icm20608_data.h"
#include "sensorworker.h"

#include <QDebug>
#include <QMessageBox>
#include <QCoreApplication>
#include <QSettings>
#include <QDateTime>
#include <QDir>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

/* 冒号后边叫成员初始化列表，进入函数体 {} 之前执行 */
MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent) /* 初始化基类窗口 */
	, ui(new Ui::MainWindow) /* Ui::MainWindow *ui = new Ui::MainWindow; */
	, ledFd(-1)
	, ledOn(false)
	, autoMode(false)
	, lightOnThreshold(20)
	, lightOffThreshold(50)
	, sampleIntervalMs(1000)
	, keyFd(-1)
	, keyNotifier(nullptr)
	, Worker(new SensorWorker)
	, recording(false)
{
	ui->setupUi(this); /* 在当前窗口上创建、配置控件，this == &w */
	ui->autoModeCheckBox->setEnabled(false);

	ui->apStatusLabel->setText("AP3216C: WAITING");
	ui->icmStatusLabel->setText("ICM20608: WAITING");

	/* 设备打开成功后，再启用手动读取按钮 */
	ui->readSensorButton->setEnabled(false);

	loadSettings();

	/* 此时采集对象及其定时器仍属于界面线程，先设置初始间隔。 */
	Worker->setSampleInterval(sampleIntervalMs);
	Worker->moveToThread(&Thread);

	/* 工作线程启动后，安排设备初始化 */
	connect(&Thread, &QThread::started,
		Worker, &SensorWorker::start,
		Qt::QueuedConnection);

	/* 工作线程结束后，在所属线程中销毁采集对象 */
	connect(&Thread, &QThread::finished,
		Worker, &QObject::deleteLater);

	/* 界面发出手动采集请求 */
	connect(this, &MainWindow::requestApRead,
		Worker, &SensorWorker::readApOnce);

	/* 运行期间，定时器间隔必须交给所属工作线程修改。 */
	connect(this, &MainWindow::requestSampleInterval,
		Worker, &SensorWorker::setSampleInterval,
		Qt::QueuedConnection);

	/* 设备就绪后启用按钮 */
	connect(Worker, &SensorWorker::ready,
		ui->readSensorButton, &QPushButton::setEnabled);

	/* 采集结果、错误信息交给界面线程处理 */
	connect(Worker, &SensorWorker::apDataReady,
		this, &MainWindow::updateApData);

	connect(Worker, &SensorWorker::apErrorOccurred,
		this, &MainWindow::showApError);

	connect(Worker, &SensorWorker::icmDataReady,
		this, &MainWindow::updateIcmData);

	connect(Worker, &SensorWorker::icmErrorOccurred,
		this, &MainWindow::showIcmError);

	Thread.start(); /* 启动工作线程 */

	keyFd = ::open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
	if (keyFd < 0) {
		qWarning() << "open key failed:" << strerror(errno);
		ui->keyLabel->setText("KEY: unavailable");
	} else {
		keyNotifier = new QSocketNotifier(
			keyFd, QSocketNotifier::Read, this); /* 启用监听可读事件 */

		connect(keyNotifier, &QSocketNotifier::activated,
			this, &MainWindow::readKeyData);

		ui->keyLabel->setText("KEY: READY");
	}

	ledFd = ::open("/dev/led0", O_WRONLY);
	if (ledFd < 0) {
		qWarning() << "open /dev/led0 failed:" << strerror(errno);

		ui->pushButton->setText("LED unavailable");
		ui->pushButton->setEnabled(false); /* LED 按钮被禁用 */
		return;
	}

	if (!setLed(false)) {
		::close(ledFd);
		ledFd = -1;

		ui->pushButton->setText("LED init failed");
		ui->pushButton->setEnabled(false);
	} else {
		ui->autoModeCheckBox->setEnabled(true); /* led初始化成功启用自动模式开关灯 */
	}
}

MainWindow::~MainWindow()
{
	qDebug() << "MainWindow destructor called"; /* 打印到串口终端 */

	Thread.quit();
	Thread.wait();
	stopRecording();

	if (keyNotifier != nullptr) {
		keyNotifier->setEnabled(false);
	}

	if (keyFd >= 0) {
		if (::close(keyFd) < 0) {
			qWarning() << "close key failed:" << strerror(errno);
		}
	}

	if (ledFd >= 0) {
		setLed(false);

		if (::close(ledFd) < 0) {
			qWarning() << "close LED failed:" << strerror(errno);
		}

		ledFd = -1;
	}

	delete ui;
}

bool MainWindow::setLed(bool on)
{
	char value = on ? '1' : '0';
	ssize_t ret;

	do {
		ret = ::write(ledFd, &value, sizeof(value));
	} while (ret < 0 && errno == EINTR);

	if (ret != static_cast<ssize_t>(sizeof(value))) {
		if (ret < 0) {
			qWarning() << "write LED failed:" << strerror(errno);
		} else {
			qWarning() << "write LED incomplete:" << ret;
		}

		return false;
	}

	ledOn = on;

	ui->pushButton->setText(ledOn ? "LED ON - Turn OFF" : "LED OFF - Turn ON");

	return true;
}

void MainWindow::controlLedByLight(int als)
{
	bool targetOn = ledOn;

	if (als <= lightOnThreshold) {
		targetOn = true;
	} else if (als >= lightOffThreshold) {
		targetOn = false;
	}

	if (targetOn == ledOn) {
		return;
	}

	if (!setLed(targetOn)) {
		ui->statusbar->showMessage(
			"Auto LED control failed", 5000);
	}
}

void MainWindow::on_pushButton_clicked()
{
	if (!setLed(!ledOn)) {
		QMessageBox::warning(
			this, "LED error", "LED write failed. Check the terminal output.");
	}
}

void MainWindow::on_exitButton_clicked()
{
	close(); /* 关闭屏幕 */
}

void MainWindow::on_readSensorButton_clicked()
{
	emit requestApRead();
}

void MainWindow::readKeyData()
{
	struct input_event event;
	ssize_t ret;

	while (true) {
		ret = ::read(keyFd, &event, sizeof(event));

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			}

			qWarning() << "read key failed:" << strerror(errno);
			keyNotifier->setEnabled(false); /* 禁用按键文件描述符的监听 */
			ui->keyLabel->setText("KEY: read error");
			return;
		}

		if (ret == 0) {
			keyNotifier->setEnabled(false);
			ui->keyLabel->setText("KEY: disconnected");
			return;
		}

		if (event.type != EV_KEY || event.code != KEY_ENTER) {
			continue;
		}

		switch (event.value) {
		case 0:
			ui->keyLabel->setText("KEY: RELEASED");
			break;

		case 1:
			ui->keyLabel->setText("KEY: PRESSED");

			if (ui->pushButton->isEnabled()) {
				if (!setLed(!ledOn)) {
					qWarning() << "key: LED control failed";
				}
			}
			break;

		default:
			break;
		}
	}
}

void MainWindow::on_autoModeCheckBox_toggled(bool checked)
{
	autoMode = checked;

	ui->pushButton->setEnabled(!autoMode);

	if (autoMode) {
		ui->statusbar->showMessage(
			"Auto mode: waiting for valid ALS data", 5000);
	} else {
		ui->statusbar->showMessage("Manual mode", 5000);
	}
}

void MainWindow::updateApData(int ir, int als, int ps, int flags)
{
	ui->apStatusLabel->setText("AP3216C: OK");
	ui->apStatusLabel->setStyleSheet("color: #89d185;");
	ui->apStatusLabel->setToolTip(QString());

	ui->irLabel->setText((flags & AP3216C_IR_VALID)
		? QString("IR: %1").arg(ir)
		: QString("IR: invalid"));

	ui->alsLabel->setText((flags & AP3216C_ALS_VALID)
		? QString("ALS: %1").arg(als)
		: QString("ALS: invalid"));

	ui->psLabel->setText((flags & AP3216C_PS_VALID)
		? QString("PS: %1").arg(ps)
		: QString("PS: invalid"));

	if (recording) {
		/* 无效通道留空，flags 保留本次各通道的有效标志。 */
		QString row = QString("%1,%2,%3,%4,%5\n")
			.arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
			.arg((flags & AP3216C_IR_VALID) ? QString::number(ir) : QString())
			.arg((flags & AP3216C_ALS_VALID) ? QString::number(als) : QString())
			.arg((flags & AP3216C_PS_VALID) ? QString::number(ps) : QString())
			.arg(flags);

		appendRecord(apRecordFile, row.toUtf8());
	}

	if (autoMode && (flags & AP3216C_ALS_VALID)) {
		controlLedByLight(als);
	}
}

void MainWindow::showApError(const QString &message)
{
	qWarning() << message;

	ui->apStatusLabel->setText("AP3216C: ERROR");
	ui->apStatusLabel->setStyleSheet("color: #f48771;");
	ui->apStatusLabel->setToolTip(message);

	ui->irLabel->setText("IR: --");
	ui->alsLabel->setText("ALS: --");
	ui->psLabel->setText("PS: --");

	ui->statusbar->showMessage(message, 5000);
}

void MainWindow::updateIcmData(int accelX, int accelY, int accelZ, int gyroX,
	int gyroY, int gyroZ, int temp)
{
	double temperature = temp / 326.8 + 25.0;

	ui->icmStatusLabel->setText("ICM20608: OK");
	ui->icmStatusLabel->setStyleSheet("color: #89d185;");
	ui->icmStatusLabel->setToolTip(QString());

	ui->accelLabel->setText(
		QString("ACC(g): X=%1 Y=%2 Z=%3")
			.arg(accelX / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3)
			.arg(accelY / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3)
			.arg(accelZ / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3));

	ui->gyroLabel->setText(
		QString("GYRO(dps): X=%1 Y=%2 Z=%3")
			.arg(gyroX / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2)
			.arg(gyroY / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2)
			.arg(gyroZ / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2));

	ui->tempLabel->setText(QString("TEMP: %1 C").arg(temperature, 0, 'f', 2));

	if (recording) {
		QString row = QString("%1,%2,%3,%4,%5,%6,%7,%8\n")
			.arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
			.arg(accelX / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3)
			.arg(accelY / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3)
			.arg(accelZ / ICM20608_ACCEL_LSB_PER_G, 0, 'f', 3)
			.arg(gyroX / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2)
			.arg(gyroY / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2)
			.arg(gyroZ / ICM20608_GYRO_LSB_PER_DPS, 0, 'f', 2)
			.arg(temperature, 0, 'f', 2);

		appendRecord(icmRecordFile, row.toUtf8());
	}
}

void MainWindow::showIcmError(const QString &message)
{
	qWarning() << message;

	ui->icmStatusLabel->setText("ICM20608: ERROR");
	ui->icmStatusLabel->setStyleSheet("color: #f48771;");
	ui->icmStatusLabel->setToolTip(message);

	ui->accelLabel->setText("ACC: --");
	ui->gyroLabel->setText("GYRO: --");
	ui->tempLabel->setText("TEMP: --");

	ui->statusbar->showMessage(message, 5000);
}

void MainWindow::loadSettings()
{
	QString path = QCoreApplication::applicationDirPath()
		+ "/device_monitor.ini";
	QSettings settings(path, QSettings::IniFormat);
	bool onOk;
	bool offOk;
	bool intervalOk;
	int onThreshold = settings.value("led/onThreshold", 20).toInt(&onOk);
	int offThreshold = settings.value("led/offThreshold", 50).toInt(&offOk);
	int interval = settings.value("sensor/intervalMs", 1000).toInt(&intervalOk);

	if (settings.status() == QSettings::NoError && onOk && offOk && intervalOk
		&& onThreshold >= 0 && offThreshold <= 65535
		&& onThreshold < offThreshold && interval >= 200 && interval <= 60000) {
		lightOnThreshold = onThreshold;
		lightOffThreshold = offThreshold;
		sampleIntervalMs = interval;
		ui->settingsFeedbackLabel->setText("Parameters loaded. Startup mode: manual.");
	} else {
		ui->settingsFeedbackLabel->setText(
			"Invalid or unreadable configuration. Using defaults: 20 / 50 / 1000 ms.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #dcdcaa;");
	}

	ui->lightOnSpinBox->setValue(lightOnThreshold);
	ui->lightOffSpinBox->setValue(lightOffThreshold);
	ui->sampleIntervalSpinBox->setValue(sampleIntervalMs);
	ui->configPathLabel->setText("Config: " + path);
}

void MainWindow::on_applySettingsButton_clicked()
{
	int onThreshold = ui->lightOnSpinBox->value();
	int offThreshold = ui->lightOffSpinBox->value();
	int interval = ui->sampleIntervalSpinBox->value();

	if (onThreshold >= offThreshold) {
		ui->settingsFeedbackLabel->setText(
			"Not applied: LED ON threshold must be lower than LED OFF threshold.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #f48771;");
		return;
	}

	lightOnThreshold = onThreshold;
	lightOffThreshold = offThreshold;
	sampleIntervalMs = interval;
	emit requestSampleInterval(sampleIntervalMs);

	QString path = QCoreApplication::applicationDirPath()
		+ "/device_monitor.ini";
	QSettings settings(path, QSettings::IniFormat);

	settings.setValue("led/onThreshold", lightOnThreshold);
	settings.setValue("led/offThreshold", lightOffThreshold);
	settings.setValue("sensor/intervalMs", sampleIntervalMs);
	settings.sync();

	if (settings.status() != QSettings::NoError) {
		ui->settingsFeedbackLabel->setText(
			"Applied for this run, but saving failed. Check the config directory permissions.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #f48771;");
		return;
	}

	ui->settingsFeedbackLabel->setText(
		"Applied and saved. Auto LED uses the next valid ALS sample.");
	ui->settingsFeedbackLabel->setStyleSheet("color: #89d185;");
	ui->statusbar->showMessage("Settings applied and saved", 5000);
}

void MainWindow::on_recordButton_clicked()
{
	if (recording) {
		stopRecording();
	} else {
		startRecording();
	}
}

void MainWindow::startRecording()
{
	QString directory = QCoreApplication::applicationDirPath() + "/records";
	QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");

	ui->recordPathLabel->setText("Path: " + directory);

	if (!QDir().mkpath(directory)) {
		stopRecording("Cannot create records directory.");
		return;
	}

	apRecordFile.setFileName(directory + "/ap3216c_" + stamp + ".csv");
	icmRecordFile.setFileName(directory + "/icm20608_" + stamp + ".csv");

	/* 每次创建新文件，已有同名记录不会被覆盖。 */
	if (!apRecordFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
		stopRecording("Open AP CSV failed: " + apRecordFile.errorString());
		return;
	}

	if (!icmRecordFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
		QString error = "Open ICM CSV failed: " + icmRecordFile.errorString();

		apRecordFile.close();
		apRecordFile.remove();
		stopRecording(error);
		return;
	}

	QByteArray apHeader = "timestamp,ir_raw,als_raw,ps_raw,flags\n";
	QByteArray icmHeader = "timestamp,accel_x_g,accel_y_g,accel_z_g,"
		"gyro_x_dps,gyro_y_dps,gyro_z_dps,temp_c\n";

	if (apRecordFile.write(apHeader) != apHeader.size()
		|| icmRecordFile.write(icmHeader) != icmHeader.size()
		|| !apRecordFile.flush() || !icmRecordFile.flush()) {
		stopRecording("Write CSV header failed. Check storage space and permissions.");
		apRecordFile.remove();
		icmRecordFile.remove();
		return;
	}

	recording = true;
	ui->recordStatusLabel->setToolTip(QString());
	ui->recordButton->setText("Stop Recording");
	ui->recordStatusLabel->setText("Recording: ON");
	ui->recordStatusLabel->setStyleSheet("color: #89d185;");
	ui->statusbar->showMessage("Recording started", 5000);
}

void MainWindow::stopRecording(const QString &error)
{
	QString message = error;

	for (QFile *file : { &apRecordFile, &icmRecordFile }) {
		if (file->isOpen()) {
			if (!file->flush() && message.isEmpty()) {
				message = "Flush CSV failed: " + file->errorString();
			}

			file->close();
		}
	}

	recording = false;
	ui->recordButton->setText("Start Recording");

	if (!message.isEmpty()) {
		qWarning() << message;
		ui->recordStatusLabel->setText("Recording: ERROR");
		ui->recordStatusLabel->setStyleSheet("color: #f48771;");
		ui->recordStatusLabel->setToolTip(message);
		ui->statusbar->showMessage(message, 5000);
		return;
	}

	ui->recordStatusLabel->setText("Recording: OFF");
	ui->recordStatusLabel->setStyleSheet("color: #d4d4d4;");
	ui->recordStatusLabel->setToolTip(QString());
}

void MainWindow::appendRecord(QFile &file, const QByteArray &row)
{
	/* 小规模记录直接写文件；每行刷新 Qt 缓冲区，及时发现写入错误。 */
	if (file.write(row) != row.size() || !file.flush()) {
		stopRecording("Write CSV failed: " + file.errorString());
	}
}
