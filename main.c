#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "bus_driver.h"
#include "aci.h"
#include "hw_config.h"

// Wozmon ROM bytes
const uint8_t wozmon_rom[0x0100] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Wozmon stub / entry
    0xE8, 0x8D, 0x11, 0xD0, 0x60, 0xA9, 0x00, 0x8D,
    0x12, 0xD0, 0xAD, 0x11, 0xD0, 0xF9, 0x00, 0x00
};

int main(void) {
    stdio_init_all();
    
    // Initialize expansion bus (sets up continuous 1 MHz PWM clock on GPIO 14, latches on GPIO 2-16)
    expansion_bus_init();
    
    // Initialize cassette interface
    aci_init();

    printf("Apple 1 Pico with External 5V Hardware Bus Initialized.\n");

    while (true) {
        // Main execution loop for 6502 emulation
        tight_loop_contents();
    }

    return 0;
}
