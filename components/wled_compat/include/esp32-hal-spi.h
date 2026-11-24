// esp32-hal-spi.h - the Arduino core's old SPI driver interface.
//
// The core kept the pre-5.0 SPI driver's interface for LovyanGFX, which
// still asks for it.  ESP-IDF 5.x has only the new driver, so this is the
// shape of the old one over the new: the bus handle is a token, and the four
// functions are the begin/end of a transaction.
//
// LovyanGFX only uses the handle as a "is this bus set up" flag - the
// transfers themselves go through ESP-IDF's spi_master driver.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spi_struct_t spi_t;

// SPI_CLK_EQU_SYSCLK is deliberately not defined here: LovyanGFX defines it
// itself, and a second definition of the same name is an error.

spi_t *spiStartBus(uint8_t spi_num, uint32_t clockDiv, uint8_t dataMode, uint8_t bitOrder);
void spiStopBus(spi_t *spi);
void spiSimpleTransaction(spi_t *spi);
void spiEndTransaction(spi_t *spi);

#ifdef __cplusplus
}
#endif
