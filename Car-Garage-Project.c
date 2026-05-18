#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

// ---------------- Pin assignments ----------------
#define SERVO_PIN   15   // GP15 - servo signal

#define BTN_INC     16   // GP16 - increment button (wired to GND, internal pull-up)
#define BTN_DEC     18   // GP18 - decrement button (wired to GND, internal pull-up)

#define LCD_RS      8    // GP8  - LCD register select
#define LCD_E       9    // GP9  - LCD enable
#define LCD_D4      10   // GP10 - LCD data 4
#define LCD_D5      11   // GP11 - LCD data 5
#define LCD_D6      12   // GP12 - LCD data 6
#define LCD_D7      13   // GP13 - LCD data 7
// LCD RW is tied to GND (write-only).

// ---------------- Behaviour settings ----------------
#define MAX_CARS    8      // counter cannot go above this
#define MESSAGE_MS  2000   // how long "Error" / "Parking Lot Full" stay on screen

// ---------------- Servo ----------------
#define SERVO_PERIOD_US 20000  // 20 ms period -> 50 Hz

static uint servo_slice;
static uint servo_chan;

static void servo_init(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    servo_slice = pwm_gpio_to_slice_num(pin);
    servo_chan  = pwm_gpio_to_channel(pin);

    // Scale the PWM clock so 1 counter tick = 1 microsecond.
    float div = (float)clock_get_hz(clk_sys) / 1000000.0f;
    pwm_set_clkdiv(servo_slice, div);
    pwm_set_wrap(servo_slice, SERVO_PERIOD_US - 1);  // 19999 ticks -> 50 Hz
    pwm_set_enabled(servo_slice, true);
}

// Map an angle 0..180 deg to a pulse width and apply it.
static void servo_write_angle(uint deg) {
    if (deg > 180) deg = 180;
    uint pulse_us = 500 + (deg * 2000) / 180;  // 500..2500 us
    pwm_set_chan_level(servo_slice, servo_chan, pulse_us);
}

// ---------------- HD44780 LCD (4-bit parallel) ----------------
static const uint lcd_data_pins[4] = { LCD_D4, LCD_D5, LCD_D6, LCD_D7 };

// Latch whatever is on the data pins into the LCD.
static void lcd_pulse_enable(void) {
    gpio_put(LCD_E, 1);
    sleep_us(1);
    gpio_put(LCD_E, 0);
    sleep_us(50);   // most commands settle in ~37 us
}

static void lcd_send_nibble(uint8_t nibble) {
    for (int i = 0; i < 4; i++)
        gpio_put(lcd_data_pins[i], (nibble >> i) & 1);
    lcd_pulse_enable();
}

// rs = false for a command, true for character data.
static void lcd_send_byte(uint8_t value, bool rs) {
    gpio_put(LCD_RS, rs);
    lcd_send_nibble(value >> 4);
    lcd_send_nibble(value & 0x0F);
}

static void lcd_command(uint8_t cmd) { lcd_send_byte(cmd, false); }
static void lcd_putc(char c)         { lcd_send_byte((uint8_t)c, true); }

static void lcd_puts(const char *s) {
    while (*s) lcd_putc(*s++);
}

// Move the cursor; row 0 or 1 on a 16x2 display.
static void lcd_set_cursor(uint row, uint col) {
    uint8_t addr = (row == 0 ? 0x00 : 0x40) + col;
    lcd_command(0x80 | addr);
}

static void lcd_init(void) {
    const uint all_pins[] = { LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7 };
    for (uint i = 0; i < 6; i++) {
        gpio_init(all_pins[i]);
        gpio_set_dir(all_pins[i], GPIO_OUT);
        gpio_put(all_pins[i], 0);
    }

    sleep_ms(50);                 // wait for the LCD to power up

    // Standard 4-bit init: force 8-bit mode three times, then switch to 4-bit.
    gpio_put(LCD_RS, 0);
    lcd_send_nibble(0x03); sleep_ms(5);
    lcd_send_nibble(0x03); sleep_us(150);
    lcd_send_nibble(0x03); sleep_us(150);
    lcd_send_nibble(0x02); sleep_us(150);   // switch to 4-bit mode

    lcd_command(0x28);   // 4-bit, 2 lines, 5x8 font
    lcd_command(0x0C);   // display on, cursor off, blink off
    lcd_command(0x06);   // entry mode: increment cursor, no shift
    lcd_command(0x01);   // clear display
    sleep_ms(2);         // clear needs ~1.5 ms
}

// Write a string to the top line, padded with spaces to wipe old text.
static void lcd_print_line(const char *s) {
    char line[17];
    int n = snprintf(line, sizeof(line), "%s", s);
    for (int i = n; i < 16; i++) line[i] = ' ';
    line[16] = '\0';
    lcd_set_cursor(0, 0);
    lcd_puts(line);
}

// Show "Cars: <counter>" on the top line.
static void show_counter(int counter) {
    char buf[17];
    snprintf(buf, sizeof(buf), "Cars: %d", counter);
    lcd_print_line(buf);
}

// Flash a temporary message, then restore the counter display.
static void show_message(const char *msg, int counter) {
    lcd_print_line(msg);
    sleep_ms(MESSAGE_MS);
    show_counter(counter);
}

// ---------------- Servo gate sequence ----------------
// Turn to 130 deg, hold 4 s, return to the 20 deg rest position.
static void run_gate_sequence(void) {
    servo_write_angle(130);
    sleep_ms(4000);
    servo_write_angle(20);
}

// ---------------- Buttons ----------------
static void button_init(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);   // idle = HIGH, pressed = LOW
}

int main() {
    stdio_init_all();

    servo_init(SERVO_PIN);
    servo_write_angle(20);   // start at the rest position

    button_init(BTN_INC);
    button_init(BTN_DEC);

    lcd_init();

    int counter = 0;
    show_counter(counter);
    printf("Ready. Cars: %d\n", counter);

    // Previous button states; true = released (HIGH).
    bool prev_inc = true;
    bool prev_dec = true;

    while (true) {
        bool inc = gpio_get(BTN_INC);   // false = pressed
        bool dec = gpio_get(BTN_DEC);

        // A press is a release -> press transition (HIGH -> LOW).
        bool inc_pressed = (prev_inc && !inc);
        bool dec_pressed = (prev_dec && !dec);

        if (inc_pressed) {
            if (counter >= MAX_CARS) {
                // Lot is full - reject the press, no gate movement.
                printf("Increment ignored - lot full (Cars: %d)\n", counter);
                show_message("Parking Lot Full", counter);
            } else {
                counter++;
                printf("Increment -> Cars: %d\n", counter);
                show_counter(counter);
                run_gate_sequence();
            }
        } else if (dec_pressed) {
            if (counter <= 0) {
                // Lot is empty - reject the press, no gate movement.
                printf("Decrement ignored - lot empty (Cars: %d)\n", counter);
                show_message("Error", counter);
            } else {
                counter--;
                printf("Decrement -> Cars: %d\n", counter);
                show_counter(counter);
                run_gate_sequence();
            }
        }

        if (inc_pressed || dec_pressed) {
            // Re-sync states after the blocking message/servo delay so the
            // same press is not counted again.
            prev_inc = gpio_get(BTN_INC);
            prev_dec = gpio_get(BTN_DEC);
        } else {
            prev_inc = inc;
            prev_dec = dec;
        }

        sleep_ms(20);   // poll interval; also acts as simple debounce
    }
}
