#ifndef __AP3216C_DATA_H
#define __AP3216C_DATA_H

#define AP3216C_IR_VALID  0x01
#define AP3216C_ALS_VALID 0x02
#define AP3216C_PS_VALID  0x04

struct ap3216c_data {
	unsigned short ir;
	unsigned short als;
	unsigned short ps;
	unsigned short flags;
};

#endif