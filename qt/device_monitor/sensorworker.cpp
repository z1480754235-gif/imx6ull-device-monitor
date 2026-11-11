#include "sensorworker.h"
#include "ap3216c_data.h"
#include "icm20608_data.h"

#include <QTimer>
#include <QDebug>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

SensorWorker::SensorWorker(QObject *parent)
	: QObject(parent)
	, apFd(-1)
	, icmFd(-1)
	, apTimer(new QTimer(this))
	, icmTimer(new QTimer(this))
{
	apTimer->setInterval(1000);
	icmTimer->setInterval(1000);

	connect(apTimer, &QTimer::timeout,
		this, &SensorWorker::readApOnce);

	connect(icmTimer, &QTimer::timeout,
		this, &SensorWorker::readIcmOnce);
}

SensorWorker::~SensorWorker()
{
	apTimer->stop();
	icmTimer->stop();

	if (apFd >= 0) {
		if (::close(apFd) < 0) {
			qWarning() << "close AP3216C failed:" << strerror(errno);
		}
	}

	if (icmFd >= 0) {
		if (::close(icmFd) < 0) {
			qWarning() << "close ICM20608 failed:" << strerror(errno);
		}
	}
}

void SensorWorker::setSampleInterval(int intervalMs)
{
	/* 活跃的定时器会按新间隔重新计时，未启动的只更新间隔。 */
	apTimer->setInterval(intervalMs);
	icmTimer->setInterval(intervalMs);
}

void SensorWorker::start()
{
	apFd = ::open("/dev/ap3216c", O_RDONLY);
	if (apFd < 0) {
		emit apErrorOccurred(
			QString("open AP3216C failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
	} else {
		apTimer->start(); /* 使用设置的采集间隔 */
		emit ready(true); /* 用来通知这个界面手动按钮启用 */
		readApOnce();
	}

	icmFd = ::open("/dev/icm20608", O_RDONLY);
	if (icmFd < 0) {
		emit icmErrorOccurred(
			QString("open ICM20608 failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
	} else {
		icmTimer->start();
		readIcmOnce();
	}
}

void SensorWorker::readApOnce()
{
	struct ap3216c_data data;
	ssize_t ret;

	do {
		ret = ::read(apFd, &data, sizeof(data));
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		emit apErrorOccurred(
			QString("read AP3216C failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	emit apDataReady(data.ir, data.als, data.ps, data.flags);
}

void SensorWorker::readIcmOnce()
{
	struct icm20608_data data;
	ssize_t ret;

	do {
		ret = ::read(icmFd, &data, sizeof(data));
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		emit icmErrorOccurred(
			QString("read ICM20608 failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	emit icmDataReady(data.accel_x, data.accel_y, data.accel_z, data.gyro_x,
		data.gyro_y, data.gyro_z, data.temp);
}
