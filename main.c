/**
AVR isp bub

@file		main.c
@author		Matej Kogovsek (matej@hamradio.si)
@copyright	GPL v2
*/

// ----------------------------------------------------------------------------
//
// Define programming parameters with the AT+ISPTARGET command. For example:
//
// AT+ISPTARGET=1e910a,32,2048,d7,f1,-,-,128,1000
//
// where the parameters are:
//
// 1. device signature (hex, 6 chars)
// 2. page size in bytes (dec)
// 3. firmware image size in bytes (dec)
// 4. lfuse (hex, 2 chars) or - to not program
// 5. hfuse (hex, 2 chars) or - to not program
// 6. efuse (hex, 2 chars) or - to not program
// 7. lock  (hex, 2 chars) or - to not program
// 8. eeprom image size in bytes (dec) or 0 to not program
// 9. eeprom image start in 24C512 (hex, 4 chars)
//
// Finally, check the parameters are correct by issuing:
//
// AT+ISPTARGET=?
//
// ----------------------------------------------------------------------------

#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <util/crc16.h>

#include "mat/spi.h"
#include "mat/i2c.h"
#include "mat/ee_24.h"
#include "mat/serque.h"

#include "hwdefs.h"
#include "isp.h"

// ----------------------------------------------------------------------------
// DEFINES
// ----------------------------------------------------------------------------

#define AT_CMD_UART 0

#if F_CPU == 1000000
	#define AT_CMD_BAUD BAUD_4800
#elif F_CPU == 8000000
	#define AT_CMD_BAUD BAUD_38400
#endif

#define BUFSIZE 128

#define BTN_THRE 25

// --- internal EEPROM address allocation ---

#define EEWA_PG_SIZE 0 // word
#define EEWA_FW_SIZE 2 // word

#define EEDA_XFUSE 4 // dword
#define EEDA_XFUSE_PRG 8 // dword

#define EEDA_SIG 12 // dword

#define EEWA_EE_OFFS 16 // word
#define EEWA_EE_SIZE 18 // word

// ----------------------------------------------------------------------------
// GLOBAL VARIABLES
// ----------------------------------------------------------------------------

uint8_t rxbuf[16];
uint8_t txbuf[8];

volatile uint8_t blink = 0;
volatile uint8_t btn_pressed = 0;

static uint8_t at_echo = 0;

static uint8_t buf1[BUFSIZE];
static uint8_t buf2[BUFSIZE];

static uint8_t* wbuf = buf1;
static uint16_t wlen = 0;

static uint8_t* rbuf = buf2;
static uint16_t rlen = 0;

static uint8_t bufdisp = 1;

static uint8_t i2c_adr = 0;

static uint8_t atbuf[2*BUFSIZE+16];
static uint16_t atbuflen = 0;

const char* fuse_name[] = {"lfuse","hfuse","efuse","lock"};

// ----------------------------------------------------------------------------
// AT commands
// ----------------------------------------------------------------------------

char atbufwr[]      PROGMEM = "AT+BUFWR="; // dddddd...
char atbufrd[]      PROGMEM = "AT+BUFRD";
char atbufrdlen[]   PROGMEM = "AT+BUFRDLEN";
char atbufswap[]    PROGMEM = "AT+BUFSWAP";
char atbufcmp[]     PROGMEM = "AT+BUFCMP";
char atbufrddisp[]  PROGMEM = "AT+BUFRDDISP="; // 0,1
char ati2cadr[]     PROGMEM = "AT+I2CADR=";	// aa
char atee24rd[]     PROGMEM = "AT+EE24RD="; // aaaaaa,len
char atee24wr[]     PROGMEM = "AT+EE24WR="; // aaaaaa
char atee24crc[]    PROGMEM = "AT+EE24CRC="; // len
char ateerd[]       PROGMEM = "AT+EERD="; // aaaaaa,len
char ateewr[]       PROGMEM = "AT+EEWR="; // aaaaaa
char atisptarget[]  PROGMEM = "AT+ISPTARGET="; // ...
char atispcon[]     PROGMEM = "AT+ISPCON";
char atispdis[]     PROGMEM = "AT+ISPDIS";
char atispsig[]     PROGMEM = "AT+ISPSIG";
char atisperase[]   PROGMEM = "AT+ISPERASE";
char atispflsrd[]   PROGMEM = "AT+ISPFLSRD="; // aaaaaa,len
char atispflswr[]   PROGMEM = "AT+ISPFLSWR="; // aaaaaa
char atispfuserd[]  PROGMEM = "AT+ISPFUSERD";
char atispfusewr[]  PROGMEM = "AT+ISPFUSEWR="; // dd,f
char atispeerd[]    PROGMEM = "AT+ISPEERD="; // aaaaaa,len
char atispeewr[]    PROGMEM = "AT+ISPEEWR="; // aaaaaa
char atispprogram[] PROGMEM = "AT+ISPPROGRAM";

PGM_P atcommands[] = {
	atbufwr,atbufrd,atbufrdlen,atbufswap,atbufcmp,atbufrddisp,
	ati2cadr,
	atee24rd,atee24wr,atee24crc,
	ateerd,ateewr,
	atisptarget,atispcon,atispdis,atispsig,atisperase,atispflsrd,atispflswr,
	atispfuserd,atispfusewr,atispeerd,atispeewr,atispprogram
};

// ----------------------------------------------------------------------------
// HELPER FUNCTIONS
// ----------------------------------------------------------------------------

void btn_init(void)
{
	DDR(BTN_PORT) &= ~_BV(BTN_BIT);
	BTN_PORT |= _BV(BTN_BIT);
}

void tmr0_init(void)
{
	TCCR0 = 5; // prescaler 1024
	TIMSK |= _BV(TOIE0);
}

void ser_endl(uint8_t n)
{
	ser_puts_P(n, PSTR("\r\n"));
}

// unsigned decimal string to u32
uint32_t udtoi(const char* s)
{
	uint32_t x = 0;

	while( isdigit((int)*s) ) {
		x *= 10;
		x += *s - '0';
		++s;
	}

	return x;
}

// unsigned hex string to u32
uint32_t uhtoi(const char* s, uint8_t n)
{
	uint32_t x = 0;

	uint8_t c = toupper((int)*s);

	while( n-- && ( isdigit((int)c) || ( (c >= 'A') && (c <= 'F') ) ) ) {
		if( isdigit((int)c) ) {
			c -= '0';
		} else {
			c -= 'A' - 10;
		}

		x *= 16;
		x += c;

		++s;
		c = toupper((int)*s);
	}

	return x;
}

// hex print buf
void hprintbuf(uint8_t* buf, uint16_t len)
{
	uint16_t i;
	for( i = 0; i < len; ++i ) {
		ser_puti_lc(AT_CMD_UART, buf[i], 16, 2, '0');
	}
	ser_endl(AT_CMD_UART);
}

void led_red(uint8_t on)
{
	if( on ) {
		LEDR_PORT |= _BV(LEDR_BIT);
	} else {
		LEDR_PORT &= ~_BV(LEDR_BIT);
	}
	DDR(LEDR_PORT) |= _BV(LEDR_BIT);
}

void led_grn(uint8_t on)
{
	if( on ) {
		LEDG_PORT |= _BV(LEDG_BIT);
	} else {
		LEDG_PORT &= ~_BV(LEDG_BIT);
	}
	DDR(LEDG_PORT) |= _BV(LEDG_BIT);
}

uint8_t bufofval(uint8_t* buf, uint16_t buflen, uint8_t val)
{
	while( buflen-- ) {
		if( *buf++ != val ) return 0;
	}
	return 1;
}

uint16_t ee24_crc(uint32_t adr, uint16_t len)
{
	uint16_t crc = 0;
	uint16_t i;
	uint8_t buf[16];

	for( i = 0; i < len; ++i) {
		wdt_reset();

		if( (i & 0x0f) == 0 ) {
			ee24_rd(adr+i, buf, sizeof(buf));
		}

		crc = _crc_xmodem_update(crc, buf[i & 0x0f]);
	}

	return crc;
}

// ----------------------------------------------------------------------------
// Target functions
// ----------------------------------------------------------------------------

void tgt_info(void)
{
	ser_puts_P(AT_CMD_UART, PSTR("sig "));
	ser_puti_lc(AT_CMD_UART, eeprom_read_dword((uint32_t*)EEDA_SIG), 16, 6, '0');
	ser_endl(AT_CMD_UART);

	ser_puts_P(AT_CMD_UART, PSTR("pgsize "));
	ser_puti(AT_CMD_UART, eeprom_read_word((uint16_t*)EEWA_PG_SIZE), 10);
	ser_endl(AT_CMD_UART);

	ser_puts_P(AT_CMD_UART, PSTR("fwsize "));
	ser_puti(AT_CMD_UART, eeprom_read_word((uint16_t*)EEWA_FW_SIZE), 10);
	ser_endl(AT_CMD_UART);

	uint8_t f;
	for( f = 0; f < 4; ++f ) {
		if( eeprom_read_byte((uint8_t*)(EEDA_XFUSE_PRG+f)) == 1 ) {
			ser_puts(AT_CMD_UART, fuse_name[f]);
			ser_putc(AT_CMD_UART, ' ');
			ser_puti_lc(AT_CMD_UART, eeprom_read_byte((uint8_t*)(EEDA_XFUSE+f)), 16, 2, '0');
			ser_endl(AT_CMD_UART);
		}
	}

	if( eeprom_read_word((uint16_t*)EEWA_EE_SIZE) ) {
		ser_puts_P(AT_CMD_UART, PSTR("eeoffs 0x"));
		ser_puti_lc(AT_CMD_UART, eeprom_read_word((uint16_t*)EEWA_EE_OFFS), 16, 4, '0');
		ser_endl(AT_CMD_UART);
		ser_puts_P(AT_CMD_UART, PSTR("eesize "));
		ser_puti(AT_CMD_UART, eeprom_read_word((uint16_t*)EEWA_EE_SIZE), 10);
		ser_endl(AT_CMD_UART);
	}
}

uint8_t tgt_prog_try(void)
{
	// page size check
	uint16_t pgsize = eeprom_read_word((uint16_t*)EEWA_PG_SIZE);
	if( (pgsize == 0) || (sizeof(atbuf) < pgsize) ) {
		ser_puts_P(AT_CMD_UART, PSTR("ERR: Invalid page size\r\n"));
		return 1;
	}

	// connect to target
	ser_puts_P(AT_CMD_UART, PSTR("Connecting...\r\n"));
	if( !isp_connect() ) {
		ser_puts_P(AT_CMD_UART, PSTR("ERR: Device not responding\r\n"));
		return 2;
	}

	// check device signature
	uint32_t sig = isp_dev_sig();
	if( sig != eeprom_read_dword((uint32_t*)EEDA_SIG) ) {
		ser_puts_P(AT_CMD_UART, PSTR("ERR: Device signature mismatch "));
		ser_puti_lc(AT_CMD_UART, sig, 16, 6, '0');
		ser_endl(AT_CMD_UART);
		return 3;
	}

	// program flash
	uint32_t fwsize = eeprom_read_word((uint16_t*)EEWA_FW_SIZE);
	if( fwsize ) {
		ser_puts_P(AT_CMD_UART, PSTR("Erasing...\r\n"));
		isp_chip_erase();
		ser_puts_P(AT_CMD_UART, PSTR("Programming flash...\r\n"));
		atbuflen = 0; // since atbuf will be hijacked for flash programming, clear atbuflen
		uint32_t adr = 0;
		while( adr < fwsize ) {
			wdt_reset();
			ser_puti_lc(AT_CMD_UART, adr, 16, 6, '0');
			ser_endl(AT_CMD_UART);

			ee24_rd(adr, atbuf, pgsize);
			if( bufofval(atbuf, pgsize, 0xff) ) continue; // skip empty pages
			isp_flash_wr(adr, atbuf, pgsize);
			uint8_t vrf;
			isp_flash_rd(adr, atbuf, pgsize, &vrf);
			if( vrf == 0 ) {
				ser_puts_P(AT_CMD_UART, PSTR("ERR: Flash verify failed\r\n"));
				return 4;
			}
			adr += pgsize;
		}
	}

	// program eeprom
	uint16_t eesize = eeprom_read_word((uint16_t*)EEWA_EE_SIZE);
	if( eesize ) {
		ser_puts_P(AT_CMD_UART, PSTR("Programming EE...\r\n"));
		uint16_t eeoffs = eeprom_read_word((uint16_t*)EEWA_EE_OFFS);

		uint16_t i;
		for( i = 0; i < eesize; ++i ) {
			wdt_reset();
			if( (i & 0x1f) == 0 ) { // load ee data in 32 byte chunks
				ee24_rd(eeoffs+i, atbuf, 32);
				ser_puti_lc(AT_CMD_UART, i, 16, 4, '0');
				ser_endl(AT_CMD_UART);
			}
			uint8_t d = atbuf[i & 0x1f];
			isp_ee_wr(i, d);
			if( isp_ee_rd(i) != d ) {
				ser_puts_P(AT_CMD_UART, PSTR("ERR: EE verify failed\r\n"));
				return 5;
			}
		}
	}

	// program fuses
	uint8_t f;
	for( f = 0; f < 4; ++f ) {
		wdt_reset();
		if( eeprom_read_byte((uint8_t*)(EEDA_XFUSE_PRG+f)) == 1 ) {

			uint8_t d = eeprom_read_byte((uint8_t*)(EEDA_XFUSE+f));

			ser_puts_P(AT_CMD_UART, PSTR("Setting "));
			ser_puts(AT_CMD_UART, fuse_name[f]);
			ser_puts_P(AT_CMD_UART, PSTR(" to "));
			ser_puti_lc(AT_CMD_UART, d, 16, 2, '0');
			ser_puts_P(AT_CMD_UART, PSTR("..."));

			uint8_t retr = 16;
			uint8_t oldf = isp_fuse_rd(f);
			while( isp_fuse_rd(f) != d ) {
				if( --retr == 0 ) {
					ser_puts_P(AT_CMD_UART, PSTR("FAIL\r\n"));
					isp_fuse_wr(f, oldf); // attempt to set old fuse
					return 6;
				}
				isp_fuse_wr(f, d);
			}
			ser_puts_P(AT_CMD_UART, PSTR("OK\r\n"));
		}
	}

	ser_puts_P(AT_CMD_UART, PSTR("Done.\r\n"));
	return 0;
}

uint8_t tgt_prog(void)
{
	uint8_t r = tgt_prog_try();
	isp_disconnect();
	return r;
}

//-----------------------------------------------------------------------------
//  AT command processing
//-----------------------------------------------------------------------------

uint8_t proc_at_cmd(const char* s)
{
	if( s[0] == 0 ) return 1;

	if( 0 == strcmp_P(s, PSTR("AT")) ) {
		return 0;
	}

	if( 0 == strcmp_P(s, PSTR("ATE0")) ) {
		at_echo = 0;
		return 0;
	}

	if( 0 == strcmp_P(s, PSTR("ATE1")) ) {
		at_echo = 1;
		return 0;
	}

	if( 0 == strcmp_P(s, PSTR("ATI")) ) {
		ser_puts_P(AT_CMD_UART, PSTR("AVR isp bub v1.0\r\n"));
		return 0;
	}

	if( 0 == strcmp_P(s, PSTR("AT$")) ) {
		uint8_t i;
		for( i = 0; i < (sizeof(atcommands)/sizeof(PGM_P)); ++i ) {
			ser_puts_P(AT_CMD_UART, atcommands[i]);
			ser_endl(AT_CMD_UART);
		}
		return 0;
	}

// --- buffer commands --------------------------------------------------------

	if( 0 == strncmp_P(s, atbufwr, strlen_P(atbufwr)) ) {
		s += strlen_P(atbufwr);

		uint16_t len = strlen(s);
		if( len % 2 ) return 1;
		len /= 2;
		if( len > BUFSIZE ) return 2;

		wlen = len;
		uint16_t i;
		for( i = 0; i < wlen; ++i ) {
			wbuf[i] = uhtoi(s, 2);
			s += 2;
		}

		return 0;
	}

	if( 0 == strcmp_P(s, atbufrd) ) {
		if( rlen == 0 ) return 1;

		hprintbuf(rbuf, rlen);

		return 0;
	}

	if( 0 == strcmp_P(s, atbufrdlen) ) {
		ser_puti(AT_CMD_UART, rlen, 10);
		ser_endl(AT_CMD_UART);

		return 0;
	}

	if( 0 == strcmp_P(s, atbufswap) ) {
		uint8_t* b = rbuf;
		uint16_t l = rlen;

		rbuf = wbuf;
		rlen = wlen;
		wbuf = b;
		wlen = l;

		return 0;
	}

	if( 0 == strcmp_P(s, atbufcmp) ) {
		if( rlen != wlen ) return 1;

		if( memcmp(rbuf, wbuf, rlen) ) return 1;

		return 0;
	}

	if( 0 == strncmp_P(s, atbufrddisp, strlen_P(atbufrddisp)) ) {
		s += strlen_P(atbufrddisp);
		if( strlen(s) != 1 ) return 1;

		if( s[0] == '1' ) { bufdisp = 1; return 0; }
		if( s[0] == '0' ) { bufdisp = 0; return 0; }
		//if( s[0] == '?' ) { ser_putc(AT_CMD_UART, '0'+bufdisp); ser_endl(AT_CMD_UART); return 0; }

		return 1;
	}

// --- generic I2C commands ---------------------------------------------------

	// only here for bus gofer programming script compatibility
	if( 0 == strncmp_P(s, ati2cadr, strlen_P(ati2cadr)) ) {
		s += strlen_P(ati2cadr);

		if( strlen(s) != 2 ) return 1;

		i2c_adr = uhtoi(s, 2);

		//EE24_I2C_ADR = i2c_adr;

		return 0;
	}

// --- I2C EEPROM commands ----------------------------------------------------

	if( 0 == strncmp_P(s, atee24rd, strlen_P(atee24rd)) ) {
		s += strlen_P(atee24rd);

		if( strlen(s) < 8 ) return 1;
		if( s[6] != ',' ) return 1;

		uint16_t adr = uhtoi(s, 6);
		s += 7;
		uint16_t len = udtoi(s);

		if( (len < 1) || (len > BUFSIZE) ) return 1;

		rlen = len;

		ee24_rd(adr, rbuf, rlen);

		if( bufdisp ) hprintbuf(rbuf, rlen);

		return 0;
	}

	if( 0 == strncmp_P(s, atee24wr, strlen_P(atee24wr)) ) {
		s += strlen_P(atee24wr);

		if( wlen == 0 ) return 1; // nothing to write
		//if( wlen > 64 ) return 1; // EE page write supports up to 64 bytes
		if( strlen(s) != 6 ) return 1;

		uint16_t adr = uhtoi(s, 6);

		ee24_wr(adr, wbuf, wlen);

		return 0;
	}

	if( 0 == strncmp_P(s, atee24crc, strlen_P(atee24crc)) ) {
		s += strlen_P(atee24crc);

		if( strlen(s) < 1 ) return 1;

		uint32_t len = udtoi(s);

		uint16_t crc = ee24_crc(0, len);

		ser_puti_lc(AT_CMD_UART, crc, 16, 4, '0');
		ser_endl(AT_CMD_UART);

		return 0;
	}

// --- internal EEPROM commands -----------------------------------------------
/*
	if( 0 == strncmp_P(s, ateerd, strlen_P(ateerd)) ) {
		s += strlen_P(ateerd);

		if( strlen(s) < 8 ) return 1;
		if( s[6] != ',' ) return 1;

		uint16_t adr = uhtoi(s, 6);
		s += 7;
		uint16_t len = udtoi(s);

		if( (len < 1) || (len > BUFSIZE) ) return 1;

		rlen = len;

		eeprom_read_block(rbuf, (uint8_t*)adr, rlen);

		if( bufdisp ) hprintbuf(rbuf, rlen);

		return 0;
	}

	if( 0 == strncmp_P(s, ateewr, strlen_P(ateewr)) ) {
		s += strlen_P(ateewr);

		if( wlen == 0 ) return 1; // nothing to write
		//if( wlen > 64 ) return 1; // EE page write supports up to 64 bytes
		if( strlen(s) != 6 ) return 1;

		uint16_t adr = uhtoi(s, 6);

		eeprom_write_block(wbuf, (uint8_t*)adr, wlen);

		return 0;
	}
*/
// --- AVR ISP commands -------------------------------------------------------

	if( 0 == strncmp_P(s, atisptarget, strlen_P(atisptarget)) ) {
		s += strlen_P(atisptarget);

		if( s[0] == '?' ) {
			tgt_info();
			return 0;
		}
		// sig
		if( strlen(s) < 6 ) return 1;
		eeprom_update_dword((uint32_t*)EEDA_SIG, uhtoi(s, 6));
		// pg size
		s = strchr(s, ',');
		if( s == 0 ) return 0;
		s += 1;
		eeprom_update_word((uint16_t*)EEWA_PG_SIZE, udtoi(s));
		// fw size
		s = strchr(s, ',');
		if( s == 0 ) return 0;
		s += 1;
		uint16_t fwsize = udtoi(s);
		eeprom_update_word((uint16_t*)EEWA_FW_SIZE, fwsize);
		// lfuse, hfuse, efuse, lock
		uint8_t f;
		for( f = 0; f < 4; ++f ) {
			s = strchr(s, ',');
			if( s == 0 ) return 0;
			s += 1;
			if( s[0] == '-' ) {
				eeprom_update_byte((uint8_t*)(EEDA_XFUSE_PRG+f), 0);
			} else {
				eeprom_update_byte((uint8_t*)(EEDA_XFUSE_PRG+f), 1);
				eeprom_update_byte((uint8_t*)(EEDA_XFUSE+f), uhtoi(s, 2));
			}
		}
		// ee size
		s = strchr(s, ',');
		if( s == 0 ) return 0;
		s += 1;
		eeprom_update_word((uint16_t*)EEWA_EE_SIZE, udtoi(s));
		eeprom_update_word((uint16_t*)EEWA_EE_OFFS, fwsize); // default offset = fwsize
		// ee offset
		s = strchr(s, ',');
		if( s == 0 ) return 0;
		s += 1;
		if( strlen(s) < 4 ) return 1;
		eeprom_update_word((uint16_t*)EEWA_EE_OFFS, uhtoi(s, 4));

		return 0;
	}

	if( 0 == strncmp_P(s, atispcon, strlen_P(atispcon)) ) {

		if( isp_connect() ) return 0;
	}

	if( 0 == strncmp_P(s, atispdis, strlen_P(atispdis)) ) {
		isp_disconnect();

		return 0;
	}

	if( 0 == strncmp_P(s, atispsig, strlen_P(atispsig)) ) {

		ser_puti_lc(AT_CMD_UART, isp_dev_sig(), 16, 6, '0');
		ser_endl(AT_CMD_UART);

		return 0;
	}

	if( 0 == strncmp_P(s, atisperase, strlen_P(atisperase)) ) {
		isp_chip_erase();

		return 0;
	}

	if( 0 == strncmp_P(s, atispflsrd, strlen_P(atispflsrd)) ) {
		s += strlen_P(atispflsrd);

		if( strlen(s) < 8 ) return 1;
		if( s[6] != ',' ) return 1;

		uint32_t adr = uhtoi(s, 6);
		s += 7;
		uint16_t len = udtoi(s);

		if( (len < 1) || (len > BUFSIZE) ) return 1;

		rlen = len;

		isp_flash_rd(adr, rbuf, rlen, 0);

		if( bufdisp ) hprintbuf(rbuf, rlen);

		return 0;
	}

	if( 0 == strncmp_P(s, atispflswr, strlen_P(atispflswr)) ) {
		s += strlen_P(atispflswr);

		if( wlen == 0 ) return 1; // nothing to write
		if( strlen(s) != 6 ) return 1;

		uint32_t adr = uhtoi(s, 6);

		isp_flash_wr(adr, wbuf, wlen);

		return 0;
	}

	if( 0 == strncmp_P(s, atispfuserd, strlen_P(atispfuserd)) ) {
		uint8_t i;
		for( i = 0; i < 4; ++i ) {
			ser_puts(AT_CMD_UART, fuse_name[i]);
			ser_putc(AT_CMD_UART, ' ');
			ser_puti_lc(AT_CMD_UART, isp_fuse_rd(i), 16, 2, '0');
			ser_endl(AT_CMD_UART);
		}

		return 0;
	}

	if( 0 == strncmp_P(s, atispfusewr, strlen_P(atispfusewr)) ) {
		s += strlen_P(atispfusewr);

		if( strlen(s) != 4 ) return 1;
		if( s[2] != ',' ) return 1;

		uint8_t d = uhtoi(s, 2);
		s += 3;
		uint8_t f = udtoi(s);

		if( f > 3 ) return 1;

		isp_fuse_wr(f, d);

		return 0;
	}

	if( 0 == strncmp_P(s, atispeerd, strlen_P(atispeerd)) ) {
		s += strlen_P(atispeerd);

		if( strlen(s) < 8 ) return 1;
		if( s[6] != ',' ) return 1;

		uint16_t adr = uhtoi(s, 6);
		s += 7;
		uint16_t len = udtoi(s);

		if( (len < 1) || (len > BUFSIZE) ) return 1;

		rlen = len;

		uint16_t i;
		for( i = 0; i < rlen; ++i ) {
			rbuf[i] = isp_ee_rd(adr+i);
		}

		if( bufdisp ) hprintbuf(rbuf, rlen);

		return 0;
	}

	if( 0 == strncmp_P(s, atispeewr, strlen_P(atispeewr)) ) {
		s += strlen_P(atispeewr);

		if( wlen == 0 ) return 1; // nothing to write
		if( strlen(s) != 6 ) return 1;

		uint16_t adr = uhtoi(s, 6);

		uint16_t i;
		for( i = 0; i < wlen; ++i ) {
			isp_ee_wr(adr+i, wbuf[i]);
		}

		return 0;
	}

	if( 0 == strncmp_P(s, atispprogram, strlen_P(atispprogram)) ) {
		ser_puti(AT_CMD_UART, tgt_prog(), 10);
		ser_endl(AT_CMD_UART);

		return 0;
	}

	return 1;
}

// ----------------------------------------------------------------------------
// MAIN
// ----------------------------------------------------------------------------

int main(void)
{
	wdt_reset();
	wdt_enable(WDTO_2S);

	ser_init(AT_CMD_UART, AT_CMD_BAUD, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
	ee24_init(I2C_100K);
	isp_init();
	btn_init();
	tmr0_init();

	sei();

	ser_puts_P(AT_CMD_UART, PSTR("RESET\r\n"));

	if( eeprom_read_word((uint16_t*)EEWA_PG_SIZE) == 0xffff ) { // eeprom not initialized
		eeprom_update_word((uint16_t*)EEWA_PG_SIZE, 0);
		eeprom_update_word((uint16_t*)EEWA_FW_SIZE, 0);
		eeprom_update_word((uint16_t*)EEWA_EE_SIZE, 0);
	}

	while( 1 ) {
		wdt_reset();

		// btn processing
		if( btn_pressed ) {
			ser_puts_P(AT_CMD_UART, PSTR("Parameters:\r\n"));
			led_red(0); // both leds off
			led_grn(0);
			blink |= _BV(LEDR_BIT); // blink red
			tgt_info();
			uint8_t ec = tgt_prog();
			blink = 0;
			led_red(ec != 0);
			led_grn(ec == 0);
			while( btn_pressed ) {
				wdt_reset();
				_delay_ms(200);
			}
		}

		// at command processing
		uint8_t d;
		if( ser_getc(AT_CMD_UART, &d) ) {

			// echo character
			if( at_echo ) { ser_putc(AT_CMD_UART, d); }

			// buffer overflow guard
			if( atbuflen >= sizeof(atbuf) ) { atbuflen = 0; }

			// execute on enter
			if( (d == '\r') || (d == '\n') ) {
				if( atbuflen ) {
					atbuf[atbuflen] = 0;
					atbuflen = 0;
					uint8_t r = proc_at_cmd((char*)atbuf);
					if( r == 0 ) ser_puts_P(AT_CMD_UART, PSTR("OK\r\n"));
					if( r == 1 ) ser_puts_P(AT_CMD_UART, PSTR("ERR\r\n"));
				}
			} else
			if( d == 0x7f ) {	// backspace
				if( atbuflen ) { --atbuflen; }
			} else {			// store character
				atbuf[atbuflen++] = toupper(d);
			}
		}
	}
}

// ----------------------------------------------------------------------------
// INTERRUPTS
// ----------------------------------------------------------------------------

ISR(TIMER0_OVF_vect) // should overflow approx 64 times per sec
{
	TCNT0 = 0x100 - (F_CPU / 0x10000);

	// LED blink processing
	static uint8_t blink_cnt = 0;

	if( ++blink_cnt == 15 ) {
		blink_cnt = 0;
		if( blink & _BV(LEDR_BIT) ) { LEDR_PORT ^= _BV(LEDR_BIT); }
		if( blink & _BV(LEDG_BIT) ) { LEDG_PORT ^= _BV(LEDG_BIT); }
	}

	// BTN processing
	static uint8_t btn_cnt = 0;

	if( (PIN(BTN_PORT) & _BV(BTN_BIT)) == 0 ) {
		if( ++btn_cnt >= BTN_THRE ) {
			btn_cnt = BTN_THRE;
			btn_pressed = 1;
		}
	} else {
		btn_cnt = 0;
		btn_pressed = 0;
	}
}
