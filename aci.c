/*
 * aci.c - "tape" commands on an SD card AND physical cassette audio (GPIO 0 / GPIO 26).
 *
 * Type C100R in the Woz Monitor to get the '*' prompt of the cassette interface:
 *
 *     *100.200W 500.600W FILE.DAT    write the ranges to SD card
 *     *100.200R FILE.DAT             read the SD file into the range(s)
 *     *100.200W                      write the ranges to physical cassette (GPIO 0)
 *     *100.200R                      read from physical cassette (GPIO 26 / ADC0)
 *     *R [FILE.DAT]                  read from SD, ranges taken from FILE.MAP (or TAPE.MAP)
 *     *L                             list SD tapes that have a .MAP file (also: DIR)
 *     *?                             help
 *     *                              (blank line) back to the Woz Monitor
 *
 * Tape files live in the "tape" directory on the card (created on the first W);
 * filenames cannot contain '/', '\\' or ':'.
 *
 * Ranges are inclusive, like the monitor. A tape file is just the raw bytes of
 * each range one after another (no header), so read ranges back in the order
 * they were written. When ranges are given without a filename, physical cassette
 * audio is used on GP0 (out) and GP26/ADC0 (in). W and R cannot be mixed on one
 * line, and the filename must come last.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "ff.h"
#include "aci.h"

#define ACI_DEFAULT_NAME "TAPE.DAT"
#define ACI_TAPE_DIR     "apple1/tape"
#define ACI_LINE_MAX     96
#define ACI_MAX_RANGES   32

/* Physical cassette interface pins (first available digital & ADC pins) */
#define TAPE_OUT_PIN        0     /* GP0  -> via 1uF-10uF cap + divider to MIC/Line-In */
#define TAPE_IN_PIN         26    /* GP26 (ADC0) -> biased at 1.65V, cap-coupled to EAR */
#define TAPE_IN_ADC_CHAN    0
#define TAPE_ADC_MIDPOINT   2048  /* 1.65V on 12-bit ADC (0..4095) */
#define TAPE_ADC_HYSTERESIS 100   /* Noise margin against tape hiss */
#define TAPE_TIMEOUT_US     30000000ULL /* 30-second timeout waiting for tape audio */

typedef struct { uint16_t start, end; } range_t;

/* Static, not on the stack: FatFs objects are large and the Pico stack is small. */
static FATFS   fs;
static FIL     fil;
static DIR     dir;
static FILINFO fno;
static uint8_t buf[256];
static char    line[ACI_LINE_MAX + 1];
static char    mapname[ACI_LINE_MAX + sizeof fno.fname + 8];
static char    mappath[sizeof mapname + 8];
static char    path[ACI_LINE_MAX + 16];
static char    mapbuf[512];                        /* .MAP text (we write <= 32*11 bytes) */
static range_t maprg[ACI_MAX_RANGES];              /* ranges parsed by read_map() */

static void say(const aci_host_t *h, const char *s) { while (*s) h->put_char(*s++); }

static void fail(const aci_host_t *h, const char *why)
{
    say(h, "ERR ");
    say(h, why);
    h->put_char('\r');
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Strictly  H{1,4} . H{1,4} (W|R)  ->  true, else the token is not a range. */
static bool parse_range(const char *t, range_t *r, char *op)
{
    unsigned v = 0, n = 0;
    int d;
    while ((d = hexval(*t)) >= 0) { v = (v << 4) | (unsigned)d; t++; n++; }
    if (n < 1 || n > 4 || *t != '.') return false;
    r->start = (uint16_t)v;
    t++;
    v = 0; n = 0;
    while ((d = hexval(*t)) >= 0) { v = (v << 4) | (unsigned)d; t++; n++; }
    if (n < 1 || n > 4) return false;
    r->end = (uint16_t)v;
    char c = *t;
    if (c >= 'a' && c <= 'z') c -= 0x20;
    if ((c != 'W' && c != 'R') || t[1] != '\0') return false;
    *op = c;
    return true;
}

/* ---- physical cassette audio routines (Apple-1 ACI 1000/2000 Hz FSK) ------- */

static inline void tape_write_cycle(uint32_t half_period_us)
{
    gpio_put(TAPE_OUT_PIN, 1);
    sleep_us(half_period_us);
    gpio_put(TAPE_OUT_PIN, 0);
    sleep_us(half_period_us);
}

static const char *tape_audio_write(const aci_host_t *h, const range_t *rg, unsigned nr)
{
    gpio_init(TAPE_OUT_PIN);
    gpio_set_dir(TAPE_OUT_PIN, GPIO_OUT);
    gpio_put(TAPE_OUT_PIN, 0);

    /* 1. 10-second leader tone at 1000 Hz (10,000 cycles * 1000 us) */
    for (unsigned i = 0; i < 10000; i++) {
        tape_write_cycle(500);
    }

    /* 2. Sync bit: one half-cycle at 2000 Hz (250 us) + one half-cycle at 1000 Hz (500 us) */
    gpio_put(TAPE_OUT_PIN, 1);
    sleep_us(250);
    gpio_put(TAPE_OUT_PIN, 0);
    sleep_us(500);

    /* 3. Write all ranges back-to-back, MSB first ('1' = 1000 Hz, '0' = 2000 Hz) */
    for (unsigned i = 0; i < nr; i++) {
        for (uint32_t addr = rg[i].start; addr <= rg[i].end; addr++) {
            uint8_t byte = h->read((uint16_t)addr);
            for (int bit = 7; bit >= 0; bit--) {
                if ((byte >> bit) & 1) {
                    tape_write_cycle(500);
                } else {
                    tape_write_cycle(250);
                }
            }
        }
    }

    gpio_put(TAPE_OUT_PIN, 0);
    return NULL;
}

/* Wait for the biased ADC waveform to cross the midpoint; returns elapsed us, or 0 on timeout. */
static uint32_t tape_wait_crossing(bool *state, uint64_t deadline_us)
{
    uint64_t t0 = time_us_64();
    while (time_us_64() < deadline_us) {
        uint16_t sample = adc_read();
        if (*state && sample < (TAPE_ADC_MIDPOINT - TAPE_ADC_HYSTERESIS)) {
            *state = false;
            return (uint32_t)(time_us_64() - t0);
        } else if (!*state && sample > (TAPE_ADC_MIDPOINT + TAPE_ADC_HYSTERESIS)) {
            *state = true;
            return (uint32_t)(time_us_64() - t0);
        }
    }
    return 0; /* Timed out */
}

static const char *tape_audio_read(const aci_host_t *h, const range_t *rg, unsigned nr)
{
    adc_init();
    adc_gpio_init(TAPE_IN_PIN);
    adc_select_input(TAPE_IN_ADC_CHAN);

    uint64_t deadline = time_us_64() + TAPE_TIMEOUT_US;
    bool state = (adc_read() > TAPE_ADC_MIDPOINT);

    /* 1. Detect 1000 Hz header tone (~100 consecutive half-cycles around 500 us) */
    unsigned header_half_cycles = 0;
    while (header_half_cycles < 100) {
        uint32_t dt = tape_wait_crossing(&state, deadline);
        if (dt == 0) return "TIMEOUT";
        if (dt > 375 && dt < 750) {
            header_half_cycles++;
        } else {
            header_half_cycles = 0;
        }
    }

    /* 2. Wait for the sync bit (first short half-cycle < 375 us after leader) */
    for (;;) {
        uint32_t dt = tape_wait_crossing(&state, deadline);
        if (dt == 0) return "TIMEOUT";
        if (dt < 375) {
            /* Consume second half of sync bit (~500 us) to align to bit 7 of first byte */
            if (tape_wait_crossing(&state, deadline) == 0) return "TIMEOUT";
            break;
        }
    }

    /* 3. Read all ranges back-to-back, MSB first */
    for (unsigned i = 0; i < nr; i++) {
        for (uint32_t addr = rg[i].start; addr <= rg[i].end; addr++) {
            uint8_t byte = 0;
            /* Refresh deadline per byte so long multi-minute loads don't time out mid-stream */
            deadline = time_us_64() + 2000000ULL;
            for (int bit = 7; bit >= 0; bit--) {
                uint32_t t1 = tape_wait_crossing(&state, deadline);
                uint32_t t2 = tape_wait_crossing(&state, deadline);
                if (t1 == 0 || t2 == 0) return "TAPE ERR";
                /* Full cycle > 750 us = 1000 Hz ('1'), <= 750 us = 2000 Hz ('0') */
                if ((t1 + t2) > 750) {
                    byte |= (uint8_t)(1u << bit);
                }
            }
            h->write((uint16_t)addr, byte);
        }
    }

    return NULL;
}

/* ---- range ("map") file that accompanies each tape file ------------------ */

/* Extension (text after the last '.' of the final path component), or NULL. */
static const char *ext_of(const char *name)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') dot = NULL;
        else if (*p == '.') dot = p;
    }
    return dot ? dot + 1 : NULL;
}

static bool is_map_name(const char *name)
{
    const char *e = ext_of(name);
    return e && !strcasecmp(e, "MAP");
}

/* FILE.DAT -> FILE.MAP, FILE -> FILE.MAP */
static void map_name(const char *name, char *out, size_t size)
{
    const char *e = ext_of(name);
    size_t base = e ? (size_t)(e - name - 1) : strlen(name);
    if (base > size - 5) base = size - 5;
    memcpy(out, name, base);
    strcpy(out + base, ".MAP");
}

/* "NAME" -> "tape/NAME" */
static void tape_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, ACI_TAPE_DIR "/%s", name);
}

/* Tape names stay inside the tape directory. */
static bool valid_tape_name(const char *name)
{
    if (!*name) return false;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    return true;
}

/* One line of a .MAP file -> range. Accepts "XXXX.YYYY" or "XXXX-YYYY" (any case,
 * 1-4 hex digits, surrounding blanks, trailing ';' comment). Comments, blank and
 * malformed lines return false. */
static bool parse_map_line(const char *s, const char *e, range_t *r)
{
    int d;
    unsigned v = 0, n = 0;

    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (s < e && (d = hexval(*s)) >= 0) { v = (v << 4) | (unsigned)d; s++; n++; }
    if (n < 1 || n > 4 || s >= e || (*s != '.' && *s != '-')) return false;
    r->start = (uint16_t)v;
    s++;

    v = 0; n = 0;
    while (s < e && (d = hexval(*s)) >= 0) { v = (v << 4) | (unsigned)d; s++; n++; }
    if (n < 1 || n > 4) return false;
    r->end = (uint16_t)v;

    while (s < e && (*s == ' ' || *s == '\t')) s++;
    if (s < e && *s != ';') return false;
    return r->end >= r->start;
}

/* Write the .MAP file for a tape file just saved. Returns an error word or NULL. */
static const char *write_map(const char *name, const range_t *rg, unsigned nr)
{
    unsigned len = 0;
    for (unsigned i = 0; i < nr; i++)
        len += (unsigned)snprintf(mapbuf + len, sizeof mapbuf - len, "%04X.%04X\r\n",
                                  (unsigned)rg[i].start, (unsigned)rg[i].end);

    map_name(name, mapname, sizeof mapname);
    tape_path(mappath, sizeof mappath, mapname);
    if (f_open(&fil, mappath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return "MAP";
    UINT done = 0;
    FRESULT fr = f_write(&fil, mapbuf, len, &done);
    FRESULT fc = f_close(&fil);
    return (fr != FR_OK || fc != FR_OK || done != len) ? "MAP" : NULL;
}

/* Read the .MAP that belongs to tape file 'name' into maprg[].
 * Returns the number of ranges found, or -1 if there is no readable .MAP. */
static int read_map(const char *name)
{
    map_name(name, mapname, sizeof mapname);
    tape_path(mappath, sizeof mappath, mapname);
    if (f_open(&fil, mappath, FA_READ) != FR_OK) return -1;
    UINT got = 0;
    FRESULT fr = f_read(&fil, mapbuf, sizeof mapbuf - 1, &got);
    f_close(&fil);
    if (fr != FR_OK) return -1;
    mapbuf[got] = '\0';

    /* If the buffer filled up, drop the last (possibly cut-off) line. */
    char *end = mapbuf + got;
    if (got == sizeof mapbuf - 1) {
        while (end > mapbuf && end[-1] != '\n' && end[-1] != '\r') end--;
        *end = '\0';
    }

    const char *q = mapbuf;
    if ((uint8_t)q[0] == 0xEF && (uint8_t)q[1] == 0xBB && (uint8_t)q[2] == 0xBF) q += 3;  /* UTF-8 BOM */

    unsigned n = 0;
    while (*q && n < ACI_MAX_RANGES) {
        const char *e = q;
        while (*e && *e != '\r' && *e != '\n') e++;
        if (parse_map_line(q, e, &maprg[n])) n++;
        q = e;
        while (*q == '\r' || *q == '\n') q++;
    }
    return (int)n;
}

/* ---- commands ------------------------------------------------------------- */

static void help(const aci_host_t *h)
{
    say(h, "100.200W 500.600W FILE.DAT  WRITE TO SD\r"
           "100.200R FILE.DAT           READ FROM SD\r"
           "100.200W                    WRITE TO CASSETTE (GP0)\r"
           "100.200R                    READ FROM CASSETTE (GP26)\r"
           "R [FILE.DAT]                READ SD USING .MAP\r"
           "L                           LIST SD TAPES\r"
           "?                           HELP\r"
           "(BLANK LINE)                BACK TO WOZMON\r");
}

/* L / DIR: list tape files that have a .MAP file, with the ranges they came from. */
static void list_files(const aci_host_t *h)
{
    if (f_mount(&fs, "0:", 1) != FR_OK) { fail(h, "NO SD"); return; }
    FRESULT dr = f_opendir(&dir, ACI_TAPE_DIR);
    if (dr != FR_OK) {
        f_mount(NULL, "0:", 0);
        if (dr == FR_NO_PATH || dr == FR_NO_FILE) say(h, "NO FILES\r");   /* no tape directory yet */
        else                                       fail(h, "DIR");
        return;
    }

    unsigned files = 0;
    static char out[sizeof fno.fname + 48];       /* room for the longest name + size text */
    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fattrib & (AM_HID | AM_SYS | AM_DIR)) continue;
        if (is_map_name(fno.fname)) continue;                 /* the range files themselves */

        int n = read_map(fno.fname);
        if (n < 0) continue;                                  /* no .MAP: not listed */

        snprintf(out, sizeof out, "%-12s %lu ($%04lX)\r", fno.fname,
                 (unsigned long)fno.fsize, (unsigned long)fno.fsize);
        say(h, out);
        files++;

        if (n == 0) { say(h, "  NO RANGES\r"); continue; }

        /* the ranges, normalised, four per line, indented */
        for (int i = 0; i < n; i++) {
            char tok[16];
            snprintf(tok, sizeof tok, "%04X.%04X", (unsigned)maprg[i].start, (unsigned)maprg[i].end);
            if (i % 4 == 0) say(h, "  ");
            say(h, tok);
            h->put_char((i % 4 == 3 || i == n - 1) ? '\r' : ' ');
        }
    }
    f_closedir(&dir);
    f_mount(NULL, "0:", 0);

    if (!files) say(h, "NO FILES\r");
    else { snprintf(out, sizeof out, "%u FILE%s\r", files, files == 1 ? "" : "S"); say(h, out); }
}

/* Execute the line the ACI ROM left in the monitor buffer ($0200). */
void aci_execute(const aci_host_t *h)
{
    /* Copy the typed line out of the monitor buffer (characters have bit 7 set). */
    unsigned len = 0;
    for (unsigned i = 0; i < 128; i++) {
        uint8_t c = h->read((uint16_t)(0x200 + i)) & 0x7F;
        if (c == '\r') break;
        if (len < ACI_LINE_MAX) line[len++] = (char)c;
    }
    line[len] = '\0';

    /* Tokenise on spaces: range tokens, plus up to two plain words. */
    range_t rg[ACI_MAX_RANGES];
    unsigned nr = 0, nw = 0;
    char op = 0;
    const char *words[2];
    bool any = false;

    char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        any = true;

        range_t r; char o;
        if (parse_range(tok, &r, &o)) {
            if (nw || nr == ACI_MAX_RANGES || (nr && o != op)) { fail(h, "SYNTAX"); return; }
            if (r.end < r.start) { fail(h, "RANGE"); return; }
            rg[nr++] = r;
            op = o;
        } else {
            if (nw == (nr ? 1u : 2u)) { fail(h, "SYNTAX"); return; }   /* filename goes last */
            words[nw++] = tok;
        }
    }
    if (!any) return;                                  /* blank-ish line: nothing to do */

    /* If ranges were given WITHOUT a filename, use the physical cassette audio routines */
    if (nr > 0 && nw == 0) {
        const char *err = (op == 'W') ? tape_audio_write(h, rg, nr)
                                      : tape_audio_read(h, rg, nr);
        if (err) fail(h, err);
        else     { say(h, "OK"); h->put_char('\r'); }
        return;
    }

    /* Decide what was asked for on the SD card. */
    const char *name;
    bool from_map = false;
    if (nr == 0) {
        if (nw == 1 && !strcmp(words[0], "?")) { help(h); return; }
        if (nw == 1 && (!strcasecmp(words[0], "L") || !strcasecmp(words[0], "DIR"))) { list_files(h); return; }
        if (!strcasecmp(words[0], "R")) {              /* R [FILE]: ranges come from FILE.MAP */
            from_map = true;
            op = 'R';
            name = (nw == 2) ? words[1] : ACI_DEFAULT_NAME;
        } else {
            fail(h, "SYNTAX");
            return;
        }
    } else {
        name = words[0];
    }

    if (!valid_tape_name(name)) { fail(h, "NAME"); return; }

    const char *err = NULL;

    if (f_mount(&fs, "0:", 1) != FR_OK) { fail(h, "NO SD"); return; }

    if (from_map) {
        int n = read_map(name);
        if (n < 0)       err = "NO MAP";
        else if (n == 0) err = "NO RANGES";
        else { nr = (unsigned)n; memcpy(rg, maprg, nr * sizeof rg[0]); }
    } else if (op == 'W') {
        if (is_map_name(name)) {                       /* would collide with its own range file */
            err = "NAME";
        } else {
            map_name(name, mapname, sizeof mapname);
            tape_path(mappath, sizeof mappath, mapname);
            f_unlink(mappath);                         /* old range info is now stale */
            f_mkdir(ACI_TAPE_DIR);                     /* first save: create the tape directory */
        }
    }

    if (!err) {
        tape_path(path, sizeof path, name);
        FRESULT fr = f_open(&fil, path, op == 'W' ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ);
        if (fr != FR_OK) {
            err = (op == 'W') ? "CREATE" : "NOT FOUND";
        } else {
            for (unsigned i = 0; i < nr && !err; i++) {
                uint32_t a = rg[i].start;
                uint32_t left = (uint32_t)rg[i].end - rg[i].start + 1;
                while (left && !err) {
                    UINT n = left > sizeof buf ? (UINT)sizeof buf : (UINT)left;
                    UINT done = 0;
                    if (op == 'W') {
                        for (UINT k = 0; k < n; k++) buf[k] = h->read((uint16_t)(a + k));
                        fr = f_write(&fil, buf, n, &done);
                        if (fr != FR_OK || done != n) err = "WRITE";
                    } else {
                        fr = f_read(&fil, buf, n, &done);
                        for (UINT k = 0; k < done; k++) h->write((uint16_t)(a + k), buf[k]);
                        if (fr != FR_OK) err = "READ";
                        else if (done != n) err = "EOF";
                    }
                    a += n;
                    left -= n;
                }
            }
            if (f_close(&fil) != FR_OK && !err) err = "CLOSE";
            if (op == 'W' && !err) err = write_map(name, rg, nr);   /* record where it came from */
        }
    }
    f_mount(NULL, "0:", 0);                            /* unmount: card can be swapped */

    if (err) fail(h, err);
    else     { say(h, "OK"); h->put_char('\r'); }
}

/* ---- ROM images from the SD card ------------------------------------------- */

/* One whitespace-separated token of a Woz Monitor style hex dump:
 * "E000:" sets the address, a two-digit hex number is a data byte, the rest is ignored. */
static void rom_token(const aci_host_t *h, const char *t, unsigned len, uint32_t *addr, int *total)
{
    unsigned v = 0, n = 0;
    int d;
    while (n < len && (d = hexval(t[n])) >= 0) { v = (v << 4) | (unsigned)d; n++; }
    if (n == 4 && len == 5 && t[4] == ':') { *addr = v; return; }
    if (n == 2 && len == 2 && *addr <= 0xFFFF) { h->write((uint16_t)(*addr), (uint8_t)v); (*addr)++; (*total)++; }
}

/* Load a ROM image from the card into memory through host->write.
 *   binary file : loaded at 'base'
 *   text file   : Woz Monitor hex dump ("E000: 4C B0 E2 ..."), bytes go to the dump's addresses
 * Returns the number of bytes loaded, or -1 no SD card, -2 file not found, -3 read error. */
int aci_load_rom(const aci_host_t *h, const char *file, uint16_t base)
{
    if (f_mount(&fs, "0:", 1) != FR_OK) return -1;
    if (f_open(&fil, file, FA_READ) != FR_OK) { f_mount(NULL, "0:", 0); return -2; }

    int total = 0, rc = 0;
    bool first = true, text = false;
    uint32_t addr = base;
    char tok[8];
    unsigned tl = 0;

    for (;;) {
        UINT n = 0;
        if (f_read(&fil, buf, sizeof buf, &n) != FR_OK) { rc = -3; break; }
        if (n == 0) break;

        if (first) {                       /* text if the first block is all printable ASCII */
            text = true;
            for (UINT k = 0; k < n; k++) {
                uint8_t c = buf[k];
                if (!((c >= 0x20 && c <= 0x7E) || (c >= 0x09 && c <= 0x0D))) { text = false; break; }
            }
            first = false;
        }

        for (UINT k = 0; k < n; k++) {
            if (!text) {
                if (addr <= 0xFFFF) { h->write((uint16_t)addr, buf[k]); addr++; total++; }
            } else if (buf[k] <= ' ') {
                if (tl && tl <= sizeof tok) rom_token(h, tok, tl, &addr, &total);
                tl = 0;
            } else if (tl < sizeof tok) {
                tok[tl++] = (char)buf[k];
            } else {
                tl = sizeof tok + 1;       /* too long to be a token: ignore it */
            }
        }
    }
    if (text && tl && tl <= sizeof tok) rom_token(h, tok, tl, &addr, &total);

    f_close(&fil);
    f_mount(NULL, "0:", 0);
    return rc ? rc : total;
}
