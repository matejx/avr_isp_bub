#ifndef MAT_EE24_H
#define MAT_EE24_H

#include <inttypes.h>

void ee24_init(uint8_t br);
uint8_t ee24_rd(uint16_t adr, uint8_t* buf, uint8_t len);
uint8_t ee24_wr(uint16_t adr, uint8_t* buf, uint8_t len);

#endif
