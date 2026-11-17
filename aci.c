#include "aci.h"

void aci_init(void) {
}

uint8_t aci_read(uint16_t address) {
    (void)address;
    return 0xFF;
}

void aci_write(uint16_t address, uint8_t data) {
    (void)address;
    (void)data;
}

void aci_execute(const aci_host_t *host) {
    (void)host;
}

int aci_autoload(const aci_host_t *host) {
    (void)host;
    return 0;
}
