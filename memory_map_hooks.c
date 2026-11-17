#include <stdint.h>
#include <stdbool.h>
#include "bus_driver.h"

// External ACI Cassette prototypes (aci.h)
extern uint8_t aci_read(uint16_t address);
extern void aci_write(uint16_t address, uint8_t data);

// Internal Emulated Memory Arrays
uint8_t ram_low[0xC000];        // $0000 - $BFFF (48KB Emulated RAM)
uint8_t basic_rom[0x1000];      // $E000 - $EFFF (Apple 1 BASIC ROM)
uint8_t future_rom[0x0F00];     // $F000 - $FEFF (Reserved ROM/RAM space)
extern const uint8_t wozmon_rom[0x0100]; // $FF00 - $FFFF (Wozmon ROM)

uint8_t cpu_read(uint16_t address) {
    if (address <= 0xBFFF) {
        return ram_low[address];
    }
    else if (address >= 0xC000 && address <= 0xC0FF) {
        return aci_read(address);           // K_ACI: Emulated cassette I/O
    }
    else if (address >= 0xC100 && address <= 0xC1FF) {
        return 0xEA;                        // K_ROM: Emulated cassette ROM stub
    }
    else if (address >= 0xC200 && address <= 0xCFFF) {
        return expansion_bus_read(address); // K_EXT: External expansion window 1
    }
    else if (address >= 0xD000 && address <= 0xD0FF) {
        return 0x00;                        // K_PIA: Motorola 6821 PIA emulated stub
    }
    else if (address >= 0xD100 && address <= 0xDFFF) {
        return expansion_bus_read(address); // K_EXT: External expansion window 2
    }
    else if (address >= 0xE000 && address <= 0xEFFF) {
        return basic_rom[address - 0xE000];
    }
    else if (address >= 0xF000 && address <= 0xFEFF) {
        return future_rom[address - 0xF000];
    }
    else {
        return wozmon_rom[address - 0xFF00];
    }
}

void cpu_write(uint16_t address, uint8_t data) {
    if (address <= 0xBFFF) {
        ram_low[address] = data;
    }
    else if (address >= 0xC000 && address <= 0xC0FF) {
        aci_write(address, data);           // K_ACI: Emulated cassette I/O
    }
    else if (address >= 0xC200 && address <= 0xCFFF) {
        expansion_bus_write(address, data); // K_EXT: External expansion window 1
    }
    else if (address >= 0xD000 && address <= 0xD0FF) {
        // K_PIA: Motorola 6821 PIA emulated write handler
    }
    else if (address >= 0xD100 && address <= 0xDFFF) {
        expansion_bus_write(address, data); // K_EXT: External expansion window 2
    }
    // ROM regions ($C100-$C1FF, $E000-$FFFF) ignore writes
}
