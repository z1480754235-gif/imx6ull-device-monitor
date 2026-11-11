#ifndef __ICM20608_DATA_H
#define __ICM20608_DATA_H

/* 加速度和陀螺仪换算值 */
#define ICM20608_ACCEL_LSB_PER_G  2048.0
#define ICM20608_GYRO_LSB_PER_DPS 16.4

struct icm20608_data {
	signed short accel_x;
	signed short accel_y;
	signed short accel_z;

	signed short temp;

	signed short gyro_x;
	signed short gyro_y;
	signed short gyro_z;
};

#endif
