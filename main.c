/*
 * Apple-1 emulator for Raspberry Pi Pico (RP2040) / Pico 2 (RP2350).
 * Console (keyboard + display) is the USB CDC serial port.
 *
 * Memory map
 *   $0000-$BFFF  RAM   (48 KB, contiguous)
 *   $C000-$C0FF  Cassette interface I/O - RESERVED. $C0F0 = emulator trap register
 *   $C100-$C1FF  Emulated cassette interface ROM (C100R), "tape" = file on SD card
 *   $C200-$CFFF  RAM
 *   $D000-$D0FF  6821 PIA (keyboard/display at $D010-$D013)
 *   $D100-$DFFF  RAM
 *   $E000-$EFFF  RAM: Integer BASIC, loaded from <APPLE1_ROOT>/roms/apple1basic.bin on the SD card at boot
 *   $F000-$FEFF  RAM
 *   $FF00-$FFFF  Woz Monitor ROM
 *
 * Edit the regions[] table to change the map.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "wozmon.h"       /* wozmon_rom[256]                              */
#include "aci_rom.h"      /* aci_rom[256]: cassette interface ROM at $C100 */
#include "aci.h"          /* SD-card "tape" command executor              */

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */
#define CPU_HZ           1022727u  /* Apple-1: 14.31818 MHz / 14                  */
#define THROTTLE         1         /* 0 = run as fast as possible                 */
#define SLICE_CYCLES     1000u     /* emulate ~1 ms, then flush output and sync   */
#define DSP_BUSY_CYCLES  0u        /* 17045 = real 60 char/s display; 0 = instant */
#define KBD_POLL_CYCLES  200u      /* min emulated cycles between USB polls       */
#define RESET_KEY        0x1D      /* Ctrl-]  acts as the Apple-1 RESET button    */
#define LED_FLASH_MS     120       /* on-board LED: flash length / gap            */
#define LED_FLASHES_BOOT 2         /* flashes at power-up                         */
#define LED_FLASHES_RESET 1        /* flashes on Ctrl-] reset                     */

#define BASIC_PATH       APPLE1_ROOT "/roms/apple1basic.bin"   /* on the SD card, loaded at $E000 at boot */
#define BASIC_BASE       0xE000

/* ------------------------------------------------------------------------ */
/* On-board LED (plain Pico / Pico 2; boards with the LED on the wireless   */
/* chip have no PICO_DEFAULT_LED_PIN and simply get no LED).                */
/* ------------------------------------------------------------------------ */
#ifdef PICO_DEFAULT_LED_PIN
static void led_init(void) { gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT); gpio_put(PICO_DEFAULT_LED_PIN, 0); }
static void led_set(bool on) { gpio_put(PICO_DEFAULT_LED_PIN, on); }
#else
static void led_init(void) {}
static void led_set(bool on) { (void)on; }
#endif

static void led_flash(unsigned n)
{
    while (n--) { led_set(true); sleep_ms(LED_FLASH_MS); led_set(false); sleep_ms(LED_FLASH_MS); }
}

/* ------------------------------------------------------------------------ */
/* Memory                                                                   */
/* ------------------------------------------------------------------------ */
enum { K_RAM = 0, K_ROM, K_PIA, K_ACI };

static uint8_t  mem[65536];
static uint8_t  kind[256];            /* what each 256-byte page is */
static uint32_t cpu_clk;              /* used by cpu6502.h and the PIA timing */

typedef struct { uint8_t first_page, last_page, kind; } region_t;

/* Applied in order, later entries override earlier ones. */
static const region_t regions[] = {
    { 0x00, 0xFF, K_RAM },    /* default: everything is RAM                */
    { 0xC0, 0xC0, K_ACI },    /* $C000-$C0FF cassette I/O (reserved)       */
    { 0xC1, 0xC1, K_ROM },    /* $C100-$C1FF cassette interface ROM        */
    { 0xD0, 0xD0, K_PIA },    /* $D000-$D0FF keyboard/display PIA          */
    { 0xFF, 0xFF, K_ROM },    /* $FF00-$FFFF Woz Monitor                   */
};

static void map_init(void)
{
    for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++)
        for (unsigned pg = regions[i].first_page; pg <= regions[i].last_page; pg++)
            kind[pg] = regions[i].kind;

    memset(mem, 0x00, sizeof mem);
    memset(&mem[0xC000], 0xFF, 0x100);               /* ACI I/O page reads as $FF */
    memcpy(&mem[0xC100], aci_rom, sizeof aci_rom);
    memcpy(&mem[0xFF00], wozmon_rom,   sizeof wozmon_rom);
}

/* ------------------------------------------------------------------------ */
/* USB serial console                                                       */
/* ------------------------------------------------------------------------ */
static char    outbuf[256];
static unsigned outlen;

static void out_flush(void)
{
    if (outlen) {
        stdio_put_string(outbuf, (int)outlen, false, false);
        outlen = 0;
    }
}

static inline void out_char(char c)
{
    if (outlen >= sizeof outbuf - 2) out_flush();
    outbuf[outlen++] = c;
}

static void out_str(const char *s) { while (*s) out_char(*s++); }

/* Character written to the Apple-1 display -> terminal */
static void dsp_putc(uint8_t v)
{
    v &= 0x7F;
    /* The Woz Monitor's reset code does STY DSP with Y=$7F. After a real RESET the PIA is
     * cleared so that write only reaches the data-direction register, but a soft reset
     * (CALL -256, JMP $FF00) arrives with the PIA already set up and it would print a
     * stray character. $7F (DEL) is never meant to be displayed, so drop it. */
    if (v == 0x7F)       return;
    if (v == 0x0D)       { out_char('\r'); out_char('\n'); }
    else if (v >= 0x60)  out_char((char)(v - 0x20));   /* display is upper case only */
    else if (v >= 0x20)  out_char((char)v);
    /* other control codes are ignored, like the real display */
}

static bool reset_req;
static int  last_in;

/* Pull one usable key from USB into the PIA latch (if any is waiting). */
static void kbd_poll(void);

/* ------------------------------------------------------------------------ */
/* 6821 PIA at $D010-$D013                                                  */
/*   $D010 KBD   keyboard data   (bit 7 always 1)                           */
/*   $D011 KBDCR keyboard ctrl   (bit 7 = key ready)                        */
/*   $D012 DSP   display data    (bit 7 = display busy when read)           */
/*   $D013 DSPCR display ctrl                                               */
/* Bit 2 of each control register selects data register (1) or DDR (0).     */
/* ------------------------------------------------------------------------ */
static struct {
    uint8_t  kbd_ddr, kbdcr, dsp_ddr, dspcr, dsp_last;
    uint8_t  key;
    bool     key_ready, dsp_busy;
    uint32_t dsp_busy_until, last_poll;
} pia;

static void kbd_poll(void)
{
    for (;;) {
        int c = getchar_timeout_us(0);
        if (c < 0) return;                       /* nothing waiting */
        c &= 0xFF;

        if (c == RESET_KEY) { reset_req = true; return; }
        if (c == '\n' && last_in == '\r') { last_in = 0; continue; }   /* CRLF -> one CR */
        last_in = c;

        if (c == '\n')                    c = '\r';
        else if (c == 0x08 || c == 0x7F)  c = '_';          /* rubout */
        else if (c >= 'a' && c <= 'z')    c -= 0x20;        /* upper case only */
        if (c > 0x5F) continue;                             /* not on an Apple-1 keyboard */

        pia.key = (uint8_t)c;
        pia.key_ready = true;
        return;
    }
}

static uint8_t pia_read(uint16_t addr)
{
    if ((addr & 0xFFF0) != 0xD010) return 0xFF;

    switch (addr & 3) {
    case 0:  /* KBD */
        if (pia.kbdcr & 4) { pia.key_ready = false; return pia.key | 0x80; }
        return pia.kbd_ddr;
    case 1:  /* KBDCR */
        if (!pia.key_ready && (uint32_t)(cpu_clk - pia.last_poll) >= KBD_POLL_CYCLES) {
            pia.last_poll = cpu_clk;
            kbd_poll();
        }
        return (uint8_t)((pia.kbdcr & 0x7F) | (pia.key_ready ? 0x80 : 0));
    case 2:  /* DSP */
        if (!(pia.dspcr & 4)) return pia.dsp_ddr;
        if (pia.dsp_busy && (int32_t)(cpu_clk - pia.dsp_busy_until) >= 0) pia.dsp_busy = false;
        return (uint8_t)((pia.dsp_last & 0x7F) | (pia.dsp_busy ? 0x80 : 0));
    default: /* DSPCR */
        return pia.dspcr;
    }
}

static void pia_write(uint16_t addr, uint8_t v)
{
    if ((addr & 0xFFF0) != 0xD010) return;

    switch (addr & 3) {
    case 0: if (!(pia.kbdcr & 4)) pia.kbd_ddr = v; break;
    case 1: pia.kbdcr = v & 0x3F; break;
    case 2:
        if (pia.dspcr & 4) {
            pia.dsp_last = v;
            dsp_putc(v);
            if (DSP_BUSY_CYCLES) { pia.dsp_busy = true; pia.dsp_busy_until = cpu_clk + DSP_BUSY_CYCLES; }
        } else {
            pia.dsp_ddr = v;      /* Woz Monitor writes $7F here at reset: no output */
        }
        break;
    case 3: pia.dspcr = v & 0x3F; break;
    }
}

/* ------------------------------------------------------------------------ */
/* Cassette interface                                                       */
/* The ROM at $C100 reads a command line into $0200, then writes to the     */
/* trap register; the SD-card "tape" operation runs here (aci.c).           */
/* Software that bit-bangs the real tape hardware at $C000-$C0FF is NOT    */
/* emulated - only the ROM's user interface.                                */
/* ------------------------------------------------------------------------ */
static uint8_t aci_mem_read(uint16_t a)             { return mem[a]; }
static void    aci_mem_write(uint16_t a, uint8_t v) { if (kind[a >> 8] == K_RAM) mem[a] = v; }
static void    aci_put(char c)                      { dsp_putc((uint8_t)c); }

static const aci_host_t aci_host = { aci_mem_read, aci_mem_write, aci_put };

static uint8_t aci_read(uint16_t addr) { (void)addr; return 0xFF; }
static void    aci_write(uint16_t addr, uint8_t v)
{
    (void)v;
    if (addr == ACI_TRAP_ADDR) {
        led_set(true);                      /* LED on while the SD card is busy */
        aci_execute(&aci_host);
        led_set(false);
    }
}

/* ------------------------------------------------------------------------ */
/* CPU bus                                                                  */
/* ------------------------------------------------------------------------ */
static uint8_t __not_in_flash_func(special_read)(uint16_t addr)
{
    return (kind[addr >> 8] == K_PIA) ? pia_read(addr) : aci_read(addr);
}

static void __not_in_flash_func(special_write)(uint16_t addr, uint8_t v)
{
    if (kind[addr >> 8] == K_PIA) pia_write(addr, v);
    else                          aci_write(addr, v);
}

static inline uint8_t bus_read(uint16_t addr)
{
    if (__builtin_expect(kind[addr >> 8] >= K_PIA, 0)) return special_read(addr);
    return mem[addr];                    /* RAM and ROM read the same way */
}

static inline void bus_write(uint16_t addr, uint8_t v)
{
    uint8_t k = kind[addr >> 8];
    if (__builtin_expect(k == K_RAM, 1)) mem[addr] = v;
    else if (k >= K_PIA)                 special_write(addr, v);
    /* K_ROM: write ignored */
}

#include "cpu6502.h"

/* ------------------------------------------------------------------------ */
static void machine_reset(void)
{
    memset(&pia, 0, sizeof pia);         /* RESET also clears the PIA registers */
    last_in = 0;
    reset_req = false;
    cpu_reset();                         /* RAM contents are left alone, as on hardware */
}

int main(void)
{
    stdio_init_all();
    led_init();
    led_flash(LED_FLASHES_BOOT);         /* power-up: sign of life even with no terminal */
    map_init();

    /* BASIC comes from the SD card and lands in RAM, so it is read/write. */
    led_set(true);
    int basic = aci_load_rom(&aci_host, BASIC_PATH, BASIC_BASE);
    led_set(false);

    while (!stdio_usb_connected()) sleep_ms(20);
    sleep_ms(300);                       /* let the terminal settle */

    out_str("\r\nAPPLE-1 ON PICO   (CTRL-] = RESET)\r\n");
    if (basic > 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "BASIC: %d BYTES AT E000 - START WITH E000R\r\n", basic);
        out_str(msg);
    } else {
        out_str("BASIC NOT LOADED: ");
        if (basic == -1)      out_str("NO SD CARD");
        else if (basic == -2) { out_str(BASIC_PATH); out_str(" NOT FOUND"); }
        else                  out_str("READ ERROR");
        out_str("\r\n");
        if (basic == 0) out_str("(FILE IS EMPTY)\r\n");
    }
    out_flush();
    machine_reset();

    uint64_t t0 = time_us_64();
    uint64_t emu_cycles = 0;

    for (;;) {
        emu_cycles += cpu_run(SLICE_CYCLES);
        out_flush();

        if (reset_req) {
            machine_reset();
            led_flash(LED_FLASHES_RESET);
            t0 = time_us_64();
            emu_cycles = 0;
            continue;
        }

#if THROTTLE
        uint64_t target = t0 + (emu_cycles * 1000000ull) / CPU_HZ;
        uint64_t now = time_us_64();
        if (now > target + 20000) t0 += now - target;       /* fell behind: resync, don't sprint */
        else while (time_us_64() < target) tight_loop_contents();
#endif
    }
}
