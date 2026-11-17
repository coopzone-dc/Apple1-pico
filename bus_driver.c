#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "bus_control.pio.h"
#include "bus_driver.h"

// Pin Definitions (GPIO 2-16; preserves GPIO 0 & GPIO 26 for aci.c cassette I/O)
#define BUS_BASE_PIN    2   // GPIO 2-9: Shared 8-bit Data/Address Bus
#define PIN_LE_LOW      10  // Latch Enable: Lower Address Byte (A0-A7) on 74AHCT573 #1
#define PIN_LE_HIGH     11  // Latch Enable: Upper Address Byte (A8-A15) on 74AHCT573 #2
#define PIN_DIR         12  // SN74LVC4245A DIR (HIGH = Read A->B, LOW = Write B->A)
#define PIN_OE          13  // SN74LVC4245A Output Enable (Active LOW)
#define PIN_PHI2        14  // PHI2 Continuous 1 MHz Clock Output (Hardware PWM)
#define PIN_RW          15  // Read/Write Control Line (HIGH = Read, LOW = Write)
#define PIN_RESET       16  // System Reset Line

// PWM configuration for 1 MHz PHI2 at 125 MHz system clock (wrap = 124 -> 125 cycles)
#define PHI2_PWM_WRAP   124
#define PHI2_PWM_HALF   62  // Rising edge at counter == 0, Falling edge at counter == 62

static PIO bus_pio = pio0;
static uint bus_sm = 0;
static uint phi2_slice_num = 0;

// Configure GPIO 14 to output a continuous 1 MHz clock using hardware PWM
static void setup_continuous_phi2_clock(void) {
    gpio_set_function(PIN_PHI2, GPIO_FUNC_PWM);
    phi2_slice_num = pwm_gpio_to_slice_num(PIN_PHI2);

    // 125 MHz / 1 MHz = 125 clock ticks per period (0..124).
    // Counter 0..61 = HIGH phase (PHI2 = 1), Counter 62..124 = LOW phase (PHI2 = 0).
    pwm_set_clkdiv(phi2_slice_num, 1.0f);
    pwm_set_wrap(phi2_slice_num, PHI2_PWM_WRAP);
    pwm_set_chan_level(phi2_slice_num, pwm_gpio_to_channel(PIN_PHI2), PHI2_PWM_HALF);
    pwm_set_enabled(phi2_slice_num, true);
}

/**
 * Synchronize bus access to the continuous PHI2 PWM clock phase.
 *
 * NOTE ON PWM EDGE SYNCHRONIZATION:
 * Because PHI2 runs continuously at 1 MHz via hardware PWM (counter 0..124):
 *   - Counter [0 .. 61]  => PHI2 is HIGH (valid data transfer phase on a real 6502)
 *   - Counter [62 .. 124] => PHI2 is LOW  (address setup phase on a real 6502)
 *
 * Waiting for the PHI2 LOW phase to latch addresses/R/W, then waiting until late in
 * the PHI2 HIGH phase (just before the falling edge at count 62) ensures external
 * cards have full address setup time and valid data hold time—mirroring a true
 * hardware 6502 bus cycle.
 */
static inline void wait_for_phi2_low_phase(void) {
    while (pwm_get_counter(phi2_slice_num) < PHI2_PWM_HALF) {
        tight_loop_contents();
    }
}

static inline void wait_for_phi2_sample_point(void) {
    // Wait until PHI2 enters HIGH phase (counter < 62)
    while (pwm_get_counter(phi2_slice_num) >= PHI2_PWM_HALF) {
        tight_loop_contents();
    }
    // Wait until near the end of PHI2 HIGH phase (around tick 52 of 62, ~80ns before falling edge)
    // so external hardware has maximum time to place valid data on the bus.
    while (pwm_get_counter(phi2_slice_num) < (PHI2_PWM_HALF - 10)) {
        tight_loop_contents();
    }
}

void expansion_bus_init(void) {
    for (int i = 0; i < 8; i++) {
        gpio_init(BUS_BASE_PIN + i);
        gpio_set_dir(BUS_BASE_PIN + i, GPIO_IN);
    }

    gpio_init(PIN_LE_LOW);  gpio_set_dir(PIN_LE_LOW, GPIO_OUT);  gpio_put(PIN_LE_LOW, 0);
    gpio_init(PIN_LE_HIGH); gpio_set_dir(PIN_LE_HIGH, GPIO_OUT); gpio_put(PIN_LE_HIGH, 0);
    gpio_init(PIN_DIR);     gpio_set_dir(PIN_DIR, GPIO_OUT);     gpio_put(PIN_DIR, 1);
    gpio_init(PIN_OE);      gpio_set_dir(PIN_OE, GPIO_OUT);      gpio_put(PIN_OE, 1);
    gpio_init(PIN_RW);      gpio_set_dir(PIN_RW, GPIO_OUT);      gpio_put(PIN_RW, 1);
    gpio_init(PIN_RESET);   gpio_set_dir(PIN_RESET, GPIO_OUT);   gpio_put(PIN_RESET, 0);

    // Start continuous 1 MHz PHI2 clock on GPIO 14
    setup_continuous_phi2_clock();

    // Load PIO program for address latching
    uint offset = pio_add_program(bus_pio, &bus_control_program);
    bus_sm = pio_claim_unused_sm(bus_pio, true);

    pio_sm_config c = bus_control_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_LE_LOW);
    sm_config_set_out_pins(&c, BUS_BASE_PIN, 8);

    pio_sm_init(bus_pio, bus_sm, offset, &c);
    pio_sm_set_enabled(bus_pio, bus_sm, true);

    sleep_ms(10);
    gpio_put(PIN_RESET, 1);
}

static inline void set_data_bus_direction(bool writing) {
    if (writing) {
        gpio_put(PIN_DIR, 0); // LOW = Port B (Pico 3.3V) -> Port A (5V Bus)
        gpio_put(PIN_RW, 0);
        for (int i = 0; i < 8; i++) {
            gpio_set_dir(BUS_BASE_PIN + i, GPIO_OUT);
        }
    } else {
        gpio_put(PIN_DIR, 1); // HIGH = Port A (5V Bus) -> Port B (Pico 3.3V)
        gpio_put(PIN_RW, 1);
        for (int i = 0; i < 8; i++) {
            gpio_set_dir(BUS_BASE_PIN + i, GPIO_IN);
        }
    }
}

void expansion_bus_write(uint16_t address, uint8_t data) {
    // 1. Align address setup with PHI2 LOW phase
    wait_for_phi2_low_phase();

    // 2. Latch Lower Address (A0-A7) and Upper Address (A8-A15)
    pio_sm_put_blocking(bus_pio, bus_sm, address & 0xFF);
    pio_sm_exec(bus_pio, bus_sm, pio_encode_jmp(bus_control_offset_latch_low));

    pio_sm_put_blocking(bus_pio, bus_sm, (address >> 8) & 0xFF);
    pio_sm_exec(bus_pio, bus_sm, pio_encode_jmp(bus_control_offset_latch_high));

    // 3. Configure Data Bus for Write and place data on GPIO 2-9
    set_data_bus_direction(true);
    for (int i = 0; i < 8; i++) {
        gpio_put(BUS_BASE_PIN + i, (data >> i) & 1);
    }
    gpio_put(PIN_OE, 0); // Enable SN74LVC4245A transceiver

    // 4. Hold data valid across the PHI2 HIGH phase until just before the falling edge
    wait_for_phi2_sample_point();

    // 5. Wait for PHI2 falling edge before isolating the data bus
    wait_for_phi2_low_phase();
    gpio_put(PIN_OE, 1); // Isolate transceiver
    gpio_put(PIN_RW, 1); // Return R/W to idle Read state
}

uint8_t expansion_bus_read(uint16_t address) {
    // 1. Align address setup with PHI2 LOW phase
    wait_for_phi2_low_phase();

    // 2. Latch Lower Address (A0-A7) and Upper Address (A8-A15)
    pio_sm_put_blocking(bus_pio, bus_sm, address & 0xFF);
    pio_sm_exec(bus_pio, bus_sm, pio_encode_jmp(bus_control_offset_latch_low));

    pio_sm_put_blocking(bus_pio, bus_sm, (address >> 8) & 0xFF);
    pio_sm_exec(bus_pio, bus_sm, pio_encode_jmp(bus_control_offset_latch_high));

    // 3. Configure Data Bus for Read and enable transceiver
    set_data_bus_direction(false);
    gpio_put(PIN_OE, 0);

    // 4. Wait until late in the PHI2 HIGH phase (just before falling edge) to sample data
    wait_for_phi2_sample_point();

    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        if (gpio_get(BUS_BASE_PIN + i)) {
            data |= (1 << i);
        }
    }

    // 5. Isolate transceiver after sampling
    gpio_put(PIN_OE, 1);
    return data;
}
