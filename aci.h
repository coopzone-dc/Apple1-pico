/* aci.h - emulated cassette interface: command-line executor using an SD card */
#ifndef ACI_H
#define ACI_H
#include <stdint.h>

/* Everything the emulator keeps on the SD card lives under this folder:
 *     /apple1/roms/apple1basic.bin     BASIC image, loaded at boot
 *     /apple1/tape/                    tape files (FILE.DAT + FILE.MAP)
 * Change it with  cmake -DAPPLE1_ROOT=name  (no leading or trailing slash). */
#ifndef APPLE1_ROOT
#define APPLE1_ROOT "apple1"
#endif

typedef struct {
    uint8_t (*read)(uint16_t addr);             /* plain memory read, no side effects  */
    void    (*write)(uint16_t addr, uint8_t v); /* store to RAM (ROM/IO silently ignored) */
    void    (*put_char)(char c);                /* send a character to the display      */
} aci_host_t;

/* Execute the command line the ACI ROM left in the monitor buffer ($0200). */
void aci_execute(const aci_host_t *host);

/* Load a ROM image from the SD card into memory: binary at 'base', or a Woz Monitor hex dump.
 * Returns bytes loaded, or -1 no SD card, -2 file not found, -3 read error. */
int aci_load_rom(const aci_host_t *host, const char *path, uint16_t base);

#endif
