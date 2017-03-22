#ifndef MAT_HWDEFS_H
#define MAT_HWDEFS_H

	#define SPI_PORT PORTB
	#define SCK_BIT 5
	#define MISO_BIT 4
	#define MOSI_BIT 3
	#define SS_BIT 2

	#define LEDR_PORT PORTB
	#define LEDR_BIT 1

	#define LEDG_PORT PORTB
	#define LEDG_BIT 0

	#define BTN_PORT PORTC
	#define BTN_BIT 0

	#define TRST_PORT PORTD
	#define TRST_BIT 5

	#define DDR(x) (*(&x - 1))
	#define PIN(x) (*(&x - 2))

#endif
