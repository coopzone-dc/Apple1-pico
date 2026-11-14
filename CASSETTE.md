# Apple-1 Pico Emulator: Physical Cassette & SD Card Interface Guide

This document covers the hardware wiring, user commands, operating procedures, and firmware architecture for the **Apple Cassette Interface (ACI)** implementation on the Raspberry Pi Pico / Pico 2 (`aci.c`).

---

## 1. Hardware Wiring & Pin Assignments

The firmware uses the first available digital GPIO pin for audio output and the first available analogue ADC pin for audio input, avoiding conflict with the SD card (`SPI0` on `GPIO 16–19`) and on-board status LED.

| Signal | Pico Pin | GPIO | Direction | Cassette Recorder Connection |
| :--- | :--- | :--- | :--- | :--- |
| **Tape Audio Out** | Pin 1 | `GP0` | Output | **MIC** or **AUX / Line-In** (via capacitor & attenuator) |
| **Tape Audio In** | Pin 31 | `GP26` (`ADC0`) | Input | **EAR / Headphone Out** (via 1.65 V DC bias network) |
| **Audio Ground** | Pin 3 / 38 | `GND` | Common | **Sleeve / Ground** of both 3.5 mm audio plugs |

### Recommended Interface Circuit

* **Output (`GP0` → Recorder `MIC` / `Line-In`):** The Pico outputs a `0–3.3 V` square wave. Because a cassette deck's `MIC` input expects roughly `10–50 mV`, connect `GP0` through a **1 µF – 10 µF DC-blocking capacitor** into a **10 kΩ potentiometer** wired as a voltage divider to `GND`. Adjust the wiper to avoid overloading the tape deck's recording level.
* **Input (Recorder `EAR` → `GP26 / ADC0`):** The Pico's ADC reads `0–3.3 V` only, whereas headphone audio swings positive and negative around `0 V`. Use two **10 kΩ resistors** between `3V3(OUT)` and `GND` to bias `GP26` at **1.65 V**, and couple the cassette player's headphone signal into `GP26` through a **1 µF – 10 µF capacitor**.

```text
  TAPE OUT (Pico -> Recorder MIC / Line-In)
  -----------------------------------------
  GP0 (Pin 1) +---[ 10k / 100ohm forming dividor]---+---[ 1uF - 10uF ]---+---> MIC / Line-In Tip
                                  |                 |
                                  +--- (Wiper) -----+---> GND (Sleeve)

  TAPE IN (Recorder Headphone -> Pico ADC0)
  -----------------------------------------
  3V3 (Pin 36) ---[ 10k ]---+---> GP26 / ADC0 (Pin 31)
                            |
  EAR Tip ------[ 1uF ]-----+
                            |
  GND (Pin 38) ---[ 10k ]---+---> GND (Sleeve)
```

---

## 2. User Operating Guide

From the Woz Monitor (`\` prompt), launch the emulated ACI ROM by typing:

```text
C100R
```

The emulator will display the `*` prompt. Commands follow Steve Wozniak's original ACI syntax, extended so that **omitting a filename routes data to the physical cassette ports**, while **providing a filename routes data to the SD card**.

### Command Reference

| Command | Target | Description |
| :--- | :--- | :--- |
| `0300.03FFW` | Physical Cassette | Record memory range `$0300–$03FF` to tape via `GP0`. |
| `0300.03FFR` | Physical Cassette | Load memory range `$0300–$03FF` from tape via `GP26`. |
| `0100.01FFW 0300.03FFW` | Physical Cassette | Record multiple ranges sequentially in a single tape pass. |
| `0300.03FFW FILE.DAT` | SD Card | Save range to `tape/FILE.DAT` and create `tape/FILE.MAP`. |
| `0300.03FFR FILE.DAT` | SD Card | Load range from `tape/FILE.DAT`. |
| `R FILE.DAT` | SD Card | Load `tape/FILE.DAT` using ranges stored in `tape/FILE.MAP`. |
| `R` | SD Card | Load default `tape/TAPE.DAT` using `tape/TAPE.MAP`. |
| `L` or `DIR` | SD Card | List all saved `.MAP` tape images and their address ranges. |
| `?` | Console | Display on-screen command summary. |
| *(Blank Line)* | System | Exit ACI prompt and return to the Woz Monitor. |

> **Important Memory Buffer Note:** The Apple-1 ACI ROM stores your typed command line in page 2 (`$0200–$027F`). Never use `$0200` as the target address when testing save/load operations, as typing the command itself will overwrite `$0200` before execution. Use `$0300` or higher.

### Recording to Cassette (`W`)

1. At the `*` prompt, type your range write command (for example, `0300.03FFW`), **without** pressing `RETURN` yet.
2. Press **RECORD + PLAY** on your cassette recorder and allow the leader tape to pass the heads for 2–3 seconds.
3. Press **`RETURN`** on the keyboard. The Pico's on-board LED will illuminate while writing:
   * First, a **10-second 1000 Hz leader tone** is generated.
   * Immediately afterward, the sync bit and data bytes are written at ~1500 baud.
4. When `OK` appears on screen and the LED turns off, press **STOP** on the recorder.

### Loading from Cassette (`R`)

1. Rewind the cassette to the start of the recording.
2. At the `*` prompt, type the matching range read command (for example, `0300.03FFR`), **without** pressing `RETURN` yet.
3. Set the cassette player volume to roughly **50%–70%** and press **PLAY**.
4. Wait 1–2 seconds until you are inside the steady 1000 Hz leader tone, then press **`RETURN`**.
5. The Pico calibrates its ADC midpoint, locks onto the leader tone, synchronizes on the sync bit, and loads the bytes into RAM. When `OK` prints, press **STOP** on the player and press **`RETURN`** (blank line) to return to Woz Monitor.

---

## 3. Apple-1 ACI Audio Protocol Specification

The firmware implements Steve Wozniak's exact Frequency Shift Keying (FSK) timing, ensuring full bidirectional compatibility with original Apple-1 hardware:

* **Header / Leader Tone:** 10,000 full cycles of **1000 Hz** (`500 µs` high, `500 µs` low = `10.0 seconds`). Allows the user to start the tape and lets the recorder's Automatic Gain Control (AGC) stabilize.
* **Sync Bit:** One half-cycle of **2000 Hz** (`250 µs` high) followed by one half-cycle of **1000 Hz** (`500 µs` low), totaling `750 µs`. Signals the transition from leader tone to byte framing.
* **Data Encoding (MSB First):** Each byte is transmitted from Bit 7 down to Bit 0:
  * **Logic `1`:** One full cycle of **1000 Hz** (`500 µs` high + `500 µs` low = `1000 µs`).
  * **Logic `0`:** One full cycle of **2000 Hz** (`250 µs` high + `250 µs` low = `500 µs`).
* **Termination:** Transmission stops immediately after Bit 0 of the final byte (no trailing checksum or end-of-file tone).

---

## 4. Firmware Routine Reference (`aci.c`)

* **`aci_execute(const aci_host_t *h)`**
  Reads the user command string from Apple-1 RAM at `$0200` (stripping bit 7), parses inclusive hex ranges (`XXXX.YYYY`) and operation flags (`W`/`R`), and checks whether a filename token is present. If `nr > 0` and no filename is given (`nw == 0`), it dispatches directly to `tape_audio_write()` or `tape_audio_read()`; otherwise, it mounts the SD card via FatFs.
* **`tape_audio_write(const aci_host_t *h, const range_t *rg, unsigned nr)`**
  Initializes `GP0` as a push-pull digital output, emits the 10,000-cycle `1000 Hz` leader and `250 µs + 500 µs` sync bit, and streams each byte from `h->read(addr)` MSB-first using microsecond-accurate `sleep_us()` delays.
* **`tape_calibrate_midpoint(uint16_t *mid_out)`**
  Samples `GP26` (`ADC0`) for 20 ms prior to leader detection to compute the exact resting DC bias voltage (`(min + max) / 2`), compensating for resistor tolerances and supply drift.
* **`tape_wait_rising_edge(uint16_t midpoint, bool *state, uint64_t deadline_us)`**
  Polls `ADC0` with a ±40-count (~32 mV) hysteresis band around the calibrated midpoint and returns the elapsed microseconds between consecutive **rising edges**. Measuring full rising-to-rising periods makes decoding immune to waveform duty-cycle asymmetry caused by tape saturation or amplifier clipping.
* **`tape_audio_read(const aci_host_t *h, const range_t *rg, unsigned nr)`**
  Orchestrates playback decoding: calibrates the DC midpoint, waits for 50 consecutive `~1000 µs` leader cycles (`850–1300 µs`), detects the `~750 µs` sync cycle (`625–850 µs`), and decodes subsequent full cycles (`> 750 µs` → `1`, `≤ 750 µs` → `0`) into RAM via `h->write(addr, byte)`.

---

## 5. Error Codes

| Error Message | Cause & Resolution |
| :--- | :--- |
| `ERR TIMEOUT` | No valid 1000 Hz leader tone or sync bit detected within 30 seconds. Increase playback volume or ensure Play was pressed before `RETURN`. |
| `ERR TAPE ERR` | Audio signal dropped out or distorted mid-transfer. Clean tape heads, check the 1.65 V bias resistors, or lower playback volume slightly if clipping. |
| `ERR SYNTAX` | Invalid range format, mixed `W` and `R` commands on one line, or invalid command keyword. |
| `ERR RANGE` | End address is lower than start address (e.g., `0400.0300W`). |
| `ERR NO SD` | SD card not inserted or failed to mount (only applies when a filename, `R`, or `L` is used). |
