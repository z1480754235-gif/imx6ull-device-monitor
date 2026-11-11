#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QSocketNotifier>
#include <QFile>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class SensorWorker;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow(QWidget *parent = nullptr);
	~MainWindow();

private slots:
	void on_pushButton_clicked();
	void on_exitButton_clicked();
	void on_readSensorButton_clicked();
	void readKeyData();
	void on_autoModeCheckBox_toggled(bool checked);
	void on_applySettingsButton_clicked();
	void on_recordButton_clicked();

	void updateApData(int ir, int als, int ps, int flags);
	void showApError(const QString &message);
	void updateIcmData(int accelX, int accelY, int accelZ, int gyroX, int gyroY,
		int gyroZ, int temp);
	void showIcmError(const QString &message);

signals:
	void requestApRead();
	void requestSampleInterval(int intervalMs);

private:
	Ui::MainWindow *ui;

	int ledFd;
	bool ledOn;
	bool autoMode;
	int lightOnThreshold;
	int lightOffThreshold;
	int sampleIntervalMs;
	int keyFd;
	QSocketNotifier *keyNotifier;

	QThread Thread;
	SensorWorker *Worker;

	bool recording;
	QFile apRecordFile;
	QFile icmRecordFile;

	bool setLed(bool on);
	void controlLedByLight(int als);
	void loadSettings();
	void startRecording();
	void stopRecording(const QString &error = QString());
	void appendRecord(QFile &file, const QByteArray &row);
};

#endif /* MAINWINDOW_H */
