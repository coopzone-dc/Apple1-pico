#ifndef BUS_DRIVER_H
#define BUS_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

// Initialize GPIO 2-16, continuous 1 MHz PWM PHI2 clock, and PIO address latches
void expansion_bus_init(void);

// External expansion bus read/write cycles synchronized with PHI2 PWM phase
void expansion_bus_write(uint16_t address, uint8_t data);
uint8_t expansion_bus_read(uint16_t address);

#endif // BUS_DRIVER_H
