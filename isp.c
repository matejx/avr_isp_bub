/**
AVR isp bub

@file		isp.c
@author		Matej Kogovsek (matej@hamradio.si)
@copyright	GPL v2
*/

#include <inttypes.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <util/delay.h>

#include "mat/spi.h"
#include "hwdefs.h"

#define ISP_FLASH_PAGE_DELAY_MS 10
#define ISP_CHIP_ERASE_DELAY_MS 20
#define ISP_FUSE_WR_DELAY_MS 10
#define ISP_EE_WR_DELAY_MS 6

#if F_CPU == 1000000
	#define ISP_SPI_FDIV SPI_FDIV_8
#elif F_CPU == 8000000
	#define ISP_SPI_FDIV SPI_FDIV_64
#endif

static const uint8_t ISP_FUSE_RD_CMD[4][2] = {
	{0x50, 0}, // lfuse
	{0x58, 8}, // hfuse
	{0x50, 8}, // efuse
	{0x58, 0}  // lock
};

static const uint8_t ISP_FUSE_WR_CMD[4] = {
	0xa0, // lfuse
	0xa8, // hfuse
	0xa4, // efuse
	0xe0  // lock
};

// --- private ----------------------------------------------------------------

void _spi_deinit(void)
{
	SPCR = 0;
	DDR(SPI_PORT) &= ~(_BV(SCK_BIT)|_BV(MOSI_BIT)|_BV(MISO_BIT));
}

uint8_t isp_prgen(void)
{
	spi_rw(0xac);
	spi_rw(0x53);
	uint8_t r = spi_rw(0);
	spi_rw(0);
	return (r == 0x53);
}

uint8_t isp_sigbyte(uint8_t n)
{
	spi_rw(0x30);
	spi_rw(0);
	spi_rw(n & 3);
	return spi_rw(0);
}

void isp_trst(uint8_t on)
{
	DDR(TRST_PORT) |= _BV(TRST_BIT);

	if( on ) {
		_spi_deinit();
		TRST_PORT |= _BV(TRST_BIT);
	} else {
		spi_init(ISP_SPI_FDIV);
		TRST_PORT &= ~_BV(TRST_BIT);
	}
}

void isp_ext_addr(uint32_t addr)
{
	spi_rw(0x4d);
	spi_rw(0);
	spi_rw(addr >> 17);
	spi_rw(0);
}

// --- public -----------------------------------------------------------------

void isp_init(void)
{
	// make SPI SS an output so it doesn't interfere with SPI
	DDR(SPI_PORT) |= _BV(SS_BIT);

	isp_trst(1);
}

uint8_t isp_connect(void)
{
	uint8_t i = 16; // retries
	uint8_t j = 1;

	while( --i ) {
		wdt_reset();
		isp_trst(0);
		_delay_ms(30);  // min 20 ms
		if( isp_prgen() ) return 1;
		isp_trst(1);
		uint8_t k = ++j; // progressively increase delay
		while( --k ) _delay_ms(10);
	}

	return 0;
}

void isp_disconnect(void)
{
	isp_trst(1);
}

uint32_t isp_dev_sig(void)
{
	uint32_t r = isp_sigbyte(0);
	r <<= 8;
	r += isp_sigbyte(1);
	r <<= 8;
	r += isp_sigbyte(2);
	return r;
}

// NOTE: non null verify pointer performs verification instead of read
void isp_flash_rd(uint32_t addr, uint8_t* pgdata, uint16_t pgsize, uint8_t* verify)
{
	if( verify ) *verify = 1;

	// load extended addr
	isp_ext_addr(addr);

	uint16_t i;
	for( i = 0; i < pgsize; ++i ) {
		if( i & 1 ) {
			spi_rw(0x28);
		} else {
			spi_rw(0x20);
		}
		spi_rw((addr+i) >> 9);
		spi_rw((addr+i) >> 1);
		uint8_t d = spi_rw(0);
		if( verify ) {
			if( *pgdata != d ) { *verify = 0; return; }
		} else {
			*pgdata = d;
		}
		++pgdata;
	}
}

void isp_flash_wr(uint32_t addr, uint8_t* pgdata, uint16_t pgsize)
{
	// fill page buffer
	uint16_t i;
	for( i = 0; i < pgsize; ++i ) {
		if( i & 1 ) {
			spi_rw(0x48);
		} else {
			spi_rw(0x40);
		}
		spi_rw(i >> 9);
		spi_rw(i >> 1);
		spi_rw(*pgdata++);
	}

	// load extended addr
	isp_ext_addr(addr);

	// program page
	spi_rw(0x4c);
	spi_rw(addr >> 9);
	spi_rw(addr >> 1);
	spi_rw(0);

	_delay_ms(ISP_FLASH_PAGE_DELAY_MS);
}

void isp_chip_erase(void)
{
	spi_rw(0xac);
	spi_rw(0x80);
	spi_rw(0);
	spi_rw(0);

	_delay_ms(ISP_CHIP_ERASE_DELAY_MS);
}

uint8_t isp_fuse_rd(uint8_t f)
{
	spi_rw(ISP_FUSE_RD_CMD[f&3][0]);
	spi_rw(ISP_FUSE_RD_CMD[f&3][1]);
	spi_rw(0);
	return spi_rw(0);
}

void isp_fuse_wr(uint8_t f, uint8_t data)
{
	spi_rw(0xac);
	spi_rw(ISP_FUSE_WR_CMD[f&3]);
	spi_rw(0);
	spi_rw(data);

	_delay_ms(ISP_FUSE_WR_DELAY_MS);
}

uint8_t isp_ee_rd(uint16_t addr)
{
	spi_rw(0xa0);
	spi_rw(addr >> 8);
	spi_rw(addr);
	return spi_rw(0);
}

void isp_ee_wr(uint16_t addr, uint8_t data)
{
	spi_rw(0xc0);
	spi_rw(addr >> 8);
	spi_rw(addr);
	spi_rw(data);

	_delay_ms(ISP_EE_WR_DELAY_MS);
}
