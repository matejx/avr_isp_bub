#ifndef MAT_ISP_H
#define MAT_ISP_H

#include <inttypes.h>

void isp_init(void);

uint8_t isp_connect(void);
void isp_disconnect(void);

uint32_t isp_dev_sig(void);

void isp_chip_erase(void);
void isp_flash_rd(uint32_t addr, uint8_t* pgdata, uint16_t pgsize, uint8_t* verify);
void isp_flash_wr(uint32_t addr, uint8_t* pgdata, uint16_t pgsize);

uint8_t isp_ee_rd(uint16_t addr);
void isp_ee_wr(uint16_t addr, uint8_t data);

#define ISP_LFUSE 0
#define ISP_HFUSE 1
#define ISP_EFUSE 2
#define ISP_LOCK  3

uint8_t isp_fuse_rd(uint8_t f);
void isp_fuse_wr(uint8_t f, uint8_t data);

#endif
