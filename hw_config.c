/*
 * SD card on SPI0 - same wiring as the RC2040 board:
 *   SCK = GPIO18, MOSI = GPIO19, MISO = GPIO16, CS = GPIO17
 *
 * Configuration for carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (the current
 * library, which supports both RP2040 and RP2350).
 */
#include "hw_config.h"

/* Hardware SPI object */
static spi_t spi = {
    .hw_inst   = spi0,
    .miso_gpio = 16,
    .mosi_gpio = 19,
    .sck_gpio  = 18,
    .baud_rate = 12500 * 1000,     /* lower this (e.g. 1000*1000) if your card is flaky */
};

/* SPI interface: which SPI the card is on and its chip select */
static sd_spi_if_t spi_if = {
    .spi     = &spi,
    .ss_gpio = 17,
};

/* The SD card socket */
static sd_card_t sd_card = {
    .type     = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num)
{
    return (num == 0) ? &sd_card : NULL;
}
