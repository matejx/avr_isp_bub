/**
@file		ee_24.c
@brief		24Cxxx EEPROM routines
@author		Matej Kogovsek
@copyright	LGPL 2.1
@note		This file is part of mat-avr-lib
@note		Tested with 24C256.
@warning	The code assumes 16 bit (2 byte) EE data addressing. Devices with more than 65kByte (or less than 256) will require code change.
*/

#include <inttypes.h>
#include <avr/io.h>

#include "mat/i2c.h"
#include "string.h"

#define EE24_I2C_ADR 0xa0 /**< EE I2C address */

/**
@brief Presently only calls i2c_init
@param[in]	br		I2C baudrate passed to i2c_init
*/
void ee24_init(uint8_t br)
{
	i2c_init(br);
}

/**
@brief Write to EE.
@param[in]	adr		Starting byte address
@param[in]	buf		Pointer to data
@param[in]	len		Number of bytes to write (len <= sizeof(buf))
@return Same as i2c_writebuf
*/
uint8_t ee24_wr(uint16_t adr, uint8_t* buf, uint8_t len)
{
	if( len > 64 ) return 1;

	uint8_t buf2[len+2];
	buf2[0] = adr >> 8;
	buf2[1] = adr;

	if( len ) memcpy(buf2+2, buf, len);

	return i2c_writebuf(EE24_I2C_ADR, buf2, len+2);
}

/**
@brief Read from EE.
@param[in]	adr		Starting byte address
@param[in]	buf		Pointer to caller allocated buffer
@param[in]	len		Number of bytes to read (len <= sizeof(buf))
@return Same as i2c_readbuf
*/
uint8_t ee24_rd(uint16_t adr, uint8_t* buf, uint8_t len)
{
	if( ee24_wr(adr, 0, 0) ) return 1;
	return i2c_readbuf(EE24_I2C_ADR, buf, len);
}
