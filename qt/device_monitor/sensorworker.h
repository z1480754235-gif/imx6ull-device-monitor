#ifndef SENSORWORKER_H
#define SENSORWORKER_H

#include <QObject>
#include <QString>

class QTimer;

class SensorWorker : public QObject
{
	Q_OBJECT

public:
	explicit SensorWorker(QObject *parent = nullptr);
	~SensorWorker() override;

public slots:
	void setSampleInterval(int intervalMs);
	void start(); /* 在线程中打开设备、启动定时器 */
	void readApOnce(); /* 读取一次AP3216C数据 */
	void readIcmOnce(); /* 读取一次ICM20608数据 */

signals:
	/* AP3216C 状态与数据 */
	void ready(bool available); /*通知界面设备已准备好 */
	void apDataReady(
		int ir, int als, int ps, int flags); /*把采集结果发送给界面 */
	void apErrorOccurred(const QString &message); /*把错误信息发送给界面 */

	/* ICM20608 数据与错误 */
	void icmDataReady(int accelX, int accelY, int accelZ, int gyroX, int gyroY,
		int gyroZ, int temp); /*把采集结果发送给界面 */
	void icmErrorOccurred(const QString &message); /*把错误信息发送给界面 */

private:
	int apFd;
	int icmFd;

	QTimer *apTimer;
	QTimer *icmTimer;
};

#endif /* SENSORWORKER_H */
