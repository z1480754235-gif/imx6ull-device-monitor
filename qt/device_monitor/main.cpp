#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
	QApplication a(argc, argv); /* 准备 Qt 应用环境 */

	MainWindow w;
	w.showFullScreen(); /* 显示屏幕，通过继承父类的成员函数 */

	return a.exec(); /* 阻塞等待输入、定时器或其他通知 */
}
