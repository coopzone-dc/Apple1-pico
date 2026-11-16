# Apple-1 Pico Cassette Interface Guide (`aci.c`)

This document describes the hardware wiring, user commands, audio protocol, and internal firmware routines for the physical cassette tape interface in `apple1_pico`.

---

## 1. Hardware Connections & Wiring

The cassette interface uses the first available digital GPIO pin (**GP0**) for FSK audio output and the first available analogue ADC pin (**GP26 / ADC0**) for audio playback input.

| Function | Pico Pin | Signal Direction | Electrical Notes |
| :--- | :--- | :--- | :--- |
| **Tape Out (`W`)** | `GP0` (Pin 1) | Pico → Cassette `MIC` / `Line-In` | 3.3 V square wave; requires DC-blocking capacitor and attenuator |
| **Tape In (`R`)** | `GP26` / `ADC0` (Pin 31) | Cassette `EAR` / Headphone → Pico | AC audio coupled onto a 1.65 V DC midpoint bias |
| **Ground** | `GND` (e.g., Pin 3 / Pin 38) | Common Ground | Connect to audio jack sleeve |

### Recommended Interface Circuit

```text
TAPE OUT (Pico -> Cassette Recorder MIC / Line-In)
--------------------------------------------------
Pico GP0 (Pin 1) ---[10k]----------[100R]-----GND
                            |
                            ||--------> Cassette MIC / Line-In Tip
                             1uF-10uF
                            +-------------------+----> Cassette GND (Sleeve)

TAPE IN (Cassette Recorder EAR / Headphone -> Pico)
---------------------------------------------------
Pico 3V3(OUT) ----[ 10k ]----+
                             |
Cassette EAR Tip ----||------+----> Pico GP26 / ADC0 (Pin 31)
                   1uF-10uF  |
Pico GND --------------------+----> [ 10k ] ---- Pico GND & Cassette Sleeve
```

* **DC Blocking (`1 µF – 10 µF`):** Prevents DC bias currents from entering your cassette deck or damaging the Pico's ADC pin.
* **Output Attenuation:** A cassette deck's `MIC` jack expects ~10–50 mV. Feeding 3.3 V directly will saturate the tape; adjust the 10 kΩ potentiometer until recordings sound clean without distortion.
* **Input Biasing:** Two equal `10 kΩ` resistors hold `GP26` at `1.65 V` (ADC reading ~2048) so negative audio swings stay safely within `0 V – 3.3 V`.

---

## 2. User Operating Guide

From the Woz Monitor (`\` prompt), launch the Apple Cassette Interface ROM by typing:

```text
C100R
```

The emulator prints `*` and waits for a command.

### Command Reference

| Command | Target | Description |
| :--- | :--- | :--- |
| `0300.03FFW` | **Physical Cassette** | Record `$0300–$03FF` to tape via `GP0` (no filename given) |
| `0300.03FFR` | **Physical Cassette** | Load `$0300–$03FF` from tape via `GP26` (no filename given) |
| `0100.01FFW 0300.03FFW` | **Physical Cassette** | Record multiple non-contiguous ranges back-to-back in one pass |
| `0300.03FFW FILE.DAT` | **SD Card** | Save `$0300–$03FF` to `/apple1/tape/FILE.DAT` + `FILE.MAP` |
| `0300.03FFR FILE.DAT` | **SD Card** | Load `/apple1/tape/FILE.DAT` into `$0300–$03FF` |
| `R [FILE.DAT]` | **SD Card** | Load `/apple1/tape/FILE.DAT` (or `TAPE.DAT`) using `.MAP` ranges |
| `L` or `DIR` | **SD Card** | List all `.MAP`-indexed tape files in `/apple1/tape` |
| `?` | Console | Print summary of available commands |
| *(Blank line / RETURN)* | Console | Exit ACI and return to the Woz Monitor (`\`) |

> **SD root folder:** `/apple1` is the default. Choose another folder at build time with `cmake -DAPPLE1_ROOT=myfolder ..` (a plain name, no slashes); BASIC loading and all tape files then use `/myfolder/roms/` and `/myfolder/tape/`.

> **Important Note on `$0200`:** Do not store test data in `$0200–$027F`. Both the Woz Monitor and the `$C100` ACI ROM use `$0200` as the live keyboard line buffer, overwriting it whenever you type a command. Use `$0300` or `$1000` for user programs and tests.

### Recording to Cassette (`W`)
1. Type `C100R` in the Woz Monitor to get the `*` prompt.
2. Press **RECORD + PLAY** on your cassette recorder.
3. Type your range command without a filename (e.g., `0300.03FFW`) and press `RETURN`.
4. The Pico's on-board LED turns on, generates a **10-second 1000 Hz leader tone**, transmits the sync bit and data bytes, outputs a trailing stop cycle, prints `OK`, and turns off the LED.
5. Press **STOP** on your cassette recorder.

### Loading from Cassette (`R`)
1. Rewind the tape to the start of the recording.
2. At the `*` prompt, type your range command without a filename (e.g., `0300.03FFR`), **do not press `RETURN` yet**.
3. Set the cassette player volume to roughly **50%–70%**.
4. Press **PLAY** on the cassette player, wait ~1–2 seconds into the leader tone, then press `RETURN`.
5. The Pico calibrates the audio midpoint, locks onto the 1000 Hz header and sync bit, loads the bytes into RAM, and prints `OK`.

---

## 3. Protocol & Firmware Routine Reference

### Apple-1 ACI Audio Protocol
`aci.c` implements Steve Wozniak's original Apple-1 FSK encoding, making recordings interchangeable with original Apple-1 hardware:
* **Leader Tone:** 10,000 cycles of **1000 Hz** (`500 µs` high + `500 µs` low = 10.0 seconds).
* **Sync Bit:** One half-cycle at **2000 Hz** (`250 µs` high) followed by one half-cycle at **1000 Hz** (`500 µs` low) = `750 µs` total.
* **Data Bits (MSB First):** Bit 7 down to Bit 0 per byte:
  * **Bit `1`:** One full cycle of **1000 Hz** (`500 µs` high + `500 µs` low = `1000 µs`).
  * **Bit `0`:** One full cycle of **2000 Hz** (`250 µs` high + `250 µs` low = `500 µs`).
* **Trailing Stop Cycle:** One extra `2000 Hz` cycle (`250 µs` high + `250 µs` low) after the final byte so the rising-edge detector has a clean closing edge for bit 0 of the last byte.

### Core Functions in `aci.c`

* **`aci_execute(const aci_host_t *h)`**
  Reads the user's command line from Apple-1 RAM at `$0200`, tokenizes memory ranges and optional filenames, and dispatches either to the physical cassette routines (`nr > 0 && nw == 0`) or the FatFs SD-card routines.
* **`tape_audio_write(const aci_host_t *h, const range_t *rg, unsigned nr)`**
  Initializes `GP0` as a digital output, emits the 10-second 1000 Hz leader and `750 µs` sync bit, streams all specified memory ranges MSB-first via `h->read()`, and appends a trailing stop cycle.
* **`tape_calibrate_midpoint(void)`**
  Samples `GP26` (`ADC0`) 400 times, 50 µs apart (~20 ms), at the start of a read operation to determine the exact resting DC bias voltage, compensating for resistor tolerances.
* **`tape_wait_rising_edge(...)`**
  Polls the 12-bit ADC with hysteresis (`±45` counts / ~36 mV, `TAPE_ADC_HYSTERESIS`) and measures full rising-edge-to-rising-edge periods in microseconds, eliminating sensitivity to asymmetric tape clipping.
* **`tape_audio_read(const aci_host_t *h, const range_t *rg, unsigned nr)`**
  Waits up to 30 seconds (from `RETURN`) for the 1000 Hz leader: 100 consecutive half-cycles each between `375` and `750 µs` (about 50 cycles, 50 ms). The deadline is then restarted and the routine waits up to 15 more seconds for the sync pulse (a half-cycle shorter than `375 µs`) and aligns to the next rising edge. It then decodes each rising-edge-to-rising-edge cycle with a `750 µs` threshold (`> 750 µs` = `1`, `<= 750 µs` = `0`) and writes each byte to RAM via `h->write()`. Because the deadline restarts once the leader locks, the tape can be started anywhere in the 30 seconds after `RETURN`.

### Error Codes

| Error Message | Cause | Fix |
| :--- | :--- | :--- |
| `ERR TIMEOUT` | No 1000 Hz leader tone detected within 30 seconds of `RETURN`, or no sync pulse within 15 seconds after the leader locked | Increase playback volume slightly, check the tape is playing, or press Play before `RETURN` |
| `ERR TAPE ERR` | Audio signal lost mid-transfer (> 2 s gap between cycles) | Ensure tape did not end early and volume is steady |
