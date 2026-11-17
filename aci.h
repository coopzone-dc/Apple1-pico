#ifndef ACI_H
#define ACI_H

#include <stdint.h>

// ACI Host structure expected by main.c
typedef struct {
    uint8_t (*read)(uint16_t addr);
    void (*write)(uint16_t addr, uint8_t val);
    void (*put)(char c);
} aci_host_t;

void aci_init(void);
uint8_t aci_read(uint16_t address);
void aci_write(uint16_t address, uint8_t data);
void aci_execute(const aci_host_t *host);
int aci_autoload(const aci_host_t *host);

#endif // ACI_H
