# Apple-1 Emulator for Raspberry Pi Pico / Pico 2 (`apple1_pico`)

An Apple-1 computer emulator running on the **Raspberry Pi Pico (RP2040)** and **Raspberry Pi Pico 2 (RP2350)**, featuring cycle-accurate 1.023 MHz 6502 CPU throttling, USB CDC serial console I/O, SD-card ROM and tape loading, and real-time audio cassette interface support.

---

## 1. Hardware Setup & Pinout

The emulator uses the Pico's **USB port** as the terminal (keyboard and display) and **SPI0** for an optional microSD card module, plus two GPIO pins for physical cassette tape I/O.

| Peripheral | Function | Raspberry Pi Pico Pin | Notes |
| :--- | :--- | :--- | :--- |
| **USB CDC** | Keyboard & Display | Micro-USB / USB-C | 115200 baud (any baud works over USB CDC) |
| **SD Card** | `MISO` | `GP16` (Pin 21) | SPI0 RX |
| **SD Card** | `CS` (`SS`) | `GP17` (Pin 22) | SPI0 CSn |
| **SD Card** | `SCK` | `GP18` (Pin 24) | SPI0 SCK |
| **SD Card** | `MOSI` | `GP19` (Pin 25) | SPI0 TX |
| **Cassette** | Audio Out (`W`) | `GP0` (Pin 1) | 1000/2000 Hz FSK output (see `CASSETTE.md`) |
| **Cassette** | Audio In (`R`) | `GP26` / `ADC0` (Pin 31) | 1.65 V DC-biased audio input |
| **Status LED** | Activity Indicator | On-board LED (`GP25`) | Flashes on boot/reset; lit during SD/tape I/O |

### SD Card Directory Layout
Format the microSD card as **FAT32** and place your files in the following structure:
```text
/
├── roms/
│   └── apple1basic.bin      # Apple-1 Integer BASIC (loaded automatically at $E000 on boot)
└── tape/
    ├── PROGRAM.DAT          # Saved binary tape images
    └── PROGRAM.MAP          # Auto-generated memory range map ("0300.03FF")
```

---

## 2. Building & Flashing

1. Ensure `PICO_SDK_PATH` is set in your environment and the FatFs submodule is present in `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`.
2. Build for **Raspberry Pi Pico (RP2040)** or **Pico 2 (RP2350)**:
   ```bash
   mkdir build && cd build
   cmake ..                     # Add -DPICO_BOARD=pico2 for Raspberry Pi Pico 2
   make -j4
   ```
3. Hold **BOOTSEL** while plugging in the Pico and copy `apple1.uf2` onto the `RPI-RP2` / `RP2350` mass-storage drive.

---

## 3. User Operating Guide

### Connecting a Serial Terminal
Open any serial terminal (PuTTY, Tera Term, `minicom`, `screen`, or `tio`) on the Pico's USB serial port. Because the emulator waits for a USB CDC host connection before booting, the welcome banner appears immediately once your terminal connects:

```text
APPLE-1 ON PICO   (CTRL-] = RESET)
BASIC: 4096 BYTES AT E000 - START WITH E000R
```

* **Reset Key (`Ctrl-]`):** Acts as the Apple-1 hardware **RESET** button. Clears the 6821 PIA registers, resets the 6502 CPU vector to `$FF00` (Woz Monitor), flashes the LED once, and preserves all RAM contents.
* **Uppercase Conversion:** Lowercase letters typed on a modern keyboard are automatically converted to uppercase to match the Apple-1's ASCII character set.
* **Backspace / Delete:** Both `Backspace` (`0x08`) and `DEL` (`0x7F`) map to the Apple-1 rubout character (`_`).

### Woz Monitor Quick Reference (`$FF00`)
Upon boot or reset, the Woz Monitor displays a `\` prompt:

| Command | Example | Description |
| :--- | :--- | :--- |
| `ADDR` | `FF00` | Examine a single byte at `$FF00` |
| `START.END` | `E000.E01F` | Dump memory range in hex |
| `ADDR: BYTE ...` | `0300: A9 41 20 EF FF 00` | Deposit bytes starting at `$0300` |
| `ADDR R` | `E000R` | Execute 6502 code starting at `$E000` (launches BASIC) |
| `C100R` | `C100R` | Enter the Apple Cassette Interface (`*` prompt; see `CASSETTE.md`) |

### Running Apple-1 Integer BASIC (`$E000`)
If `roms/apple1basic.bin` was found on the SD card at boot:
1. Type `E000R` at the `\` prompt to cold-start Integer BASIC (`>` prompt).
2. If you press `Ctrl-]` to return to the Woz Monitor, type `E2B3R` to warm-start BASIC without erasing your program in RAM.

---

## 4. System Memory Map

The memory map is configured in `main.c` via the `regions[]` table and decoded on a per-256-byte-page basis (`kind[256]`):

| Address Range | Page Kind | Size | Description |
| :--- | :--- | :--- | :--- |
| `$0000 – $BFFF` | `K_RAM` | 48 KB | Contiguous user RAM (Zero Page `$00`, Stack `$01`, Monitor Buffer `$0200`) |
| `$C000 – $C0FF` | `K_ACI` | 256 B | Cassette I/O space (`$C0F0` = `ACI_TRAP_ADDR` firmware hook) |
| `$C100 – $C1FF` | `K_ROM` | 256 B | Emulated Apple Cassette Interface ROM (`aci_rom.h`) |
| `$C200 – $CFFF` | `K_RAM` | 3.5 KB | Contiguous user RAM |
| `$D000 – $D0FF` | `K_PIA` | 256 B | Motorola 6821 PIA (`$D010–$D013` keyboard & display I/O) |
| `$D100 – $DFFF` | `K_RAM` | 3.75 KB | Contiguous user RAM |
| `$E000 – $EFFF` | `K_RAM` | 4 KB | Read/write RAM preloaded with `roms/apple1basic.bin` at boot |
| `$F000 – $FEFF` | `K_RAM` | 3.75 KB | Contiguous user RAM (available for Krusader or custom ROMs) |
| `$FF00 – $FFFF` | `K_ROM` | 256 B | Steve Wozniak's Woz Monitor ROM (`wozmon.h`) |

---

## 5. Firmware Architecture & Main Functions (`main.c`)

### Memory & Bus Emulation
* **`map_init()`**
  Populates the 256-entry `kind[]` page lookup table from `regions[]`, zeroes all 64 KB of `mem[]`, fills `$C000–$C0FF` with `$FF`, and copies the embedded `aci_rom` (`$C100`) and `wozmon_rom` (`$FF00`) images into place.
* **`bus_read(uint16_t addr)` / `bus_write(uint16_t addr, uint8_t v)`**
  Fast-path memory accessors called by `cpu6502.h` on every 6502 bus cycle. Uses `kind[addr >> 8]` with `__builtin_expect` branch hints so standard RAM/ROM accesses execute in a single array lookup, while `K_PIA` and `K_ACI` pages route to `special_read()` / `special_write()` (placed in SRAM via `__not_in_flash_func`).

### 6821 PIA & USB Console (`$D010 – $D013`)
* **`kbd_poll()`**
  Polls `getchar_timeout_us(0)` for incoming USB CDC characters. Intercepts `Ctrl-]` (`0x1D`) to trigger a machine reset, normalizes `LF`/`CRLF` into Apple-1 `CR` (`0x0D`), converts lowercase ASCII to uppercase, and latches valid keys into `pia.key`.
* **`pia_read(uint16_t addr)` / `pia_write(uint16_t addr, uint8_t v)`**
  Emulates the four 6821 PIA registers (`KBD`, `KBDCR`, `DSP`, `DSPCR`), including bit 2 Data Direction Register (DDR) selection and optional 60 chars/sec display busy timing (`DSP_BUSY_CYCLES`).
* **`dsp_putc(uint8_t v)` / `out_flush()`**
  Buffers characters written to `$D012` (`DSP`) into `outbuf[256]`, filters out Woz Monitor reset `$7F` dummy writes, expands `CR` to `CRLF`, and flushes to USB CDC at the end of each 1 ms emulation slice.

### Cassette Trap & Execution Loop
* **`aci_write(uint16_t addr, uint8_t v)`**
  Monitors writes to the `$C000–$C0FF` I/O page. When the `$C100` ROM finishes reading a command line into `$0200` and writes to `ACI_TRAP_ADDR` (`$C0F0`), this function illuminates the status LED and invokes `aci_execute(&aci_host)` in `aci.c`.
* **`machine_reset()`**
  Clears the 6821 PIA state and invokes `cpu_reset()` to load the 6502 Program Counter from `$FFFC–$FFFD`.
* **`main()`**
  Initializes USB stdio and the status LED, builds the memory map, loads `roms/apple1basic.bin` from the SD card into `$E000`, waits for a USB terminal connection, and enters the main emulation loop. Executes `cpu_run(SLICE_CYCLES)` (1,000 cycles ≈ 1 ms per slice) and paces execution against `time_us_64()` so the virtual 6502 runs at the authentic **1.022727 MHz** Apple-1 clock speed.
