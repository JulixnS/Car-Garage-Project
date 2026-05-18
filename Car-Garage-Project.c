#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"

// ---- Pins (GPIO numbers) ----
#define SERVO_PIN 15   // servo signal wire
#define BTN_INC   16   // "car in" button  (other leg wired to GND)
#define BTN_DEC   18   // "car out" button (other leg wired to GND)
#define LCD_RS    8
#define LCD_E     9
#define LCD_D4    10
#define LCD_D5    11
#define LCD_D6    12
#define LCD_D7    13

#define MAX_CARS    8      // the garage is full at this many cars
#define MESSAGE_MS  2000   // how long pop-up messages stay on screen

int servo_slice;   // which PWM block controls the servo pin

// Point the servo at an angle from 0 to 180 degrees.
void servo_angle(int degrees) {
    // A servo reads a pulse from 500us (0 deg) to 2500us (180 deg).
    int pulse = 500 + degrees * 2000 / 180;
    pwm_set_chan_level(servo_slice, pwm_gpio_to_channel(SERVO_PIN), pulse);
}

// Put 4 bits on the LCD data pins, then pulse Enable so the LCD reads them.
void lcd_nibble(int bits) {
    gpio_put(LCD_D4, bits & 1);
    gpio_put(LCD_D5, bits >> 1 & 1);
    gpio_put(LCD_D6, bits >> 2 & 1);
    gpio_put(LCD_D7, bits >> 3 & 1);
    gpio_put(LCD_E, 1);
    sleep_us(1);
    gpio_put(LCD_E, 0);
    sleep_us(50);
}

// Send one byte to the LCD. rs = 0 for a command, rs = 1 for a letter.
void lcd_send(int value, int rs) {
    gpio_put(LCD_RS, rs);
    lcd_nibble(value >> 4);     // high 4 bits first
    lcd_nibble(value & 0x0F);   // then low 4 bits
}

// Show text on the top line, padding with spaces to wipe the old text.
void lcd_print(const char *text) {
    lcd_send(0x80, 0);          // 0x80 = move cursor to start of top line
    for (int i = 0; i < 16; i++) {
        if (*text) {
            lcd_send(*text, 1);
            text++;
        } else {
            lcd_send(' ', 1);
        }
    }
}

// Show "Cars: <count>" on the LCD.
void show_count(int count) {
    char text[17];
    sprintf(text, "Cars: %d", count);
    lcd_print(text);
}

// Open the gate to 130 deg, hold 4 seconds, then close back to 20 deg.
void run_gate(void) {
    servo_angle(130);
    sleep_ms(4000);
    servo_angle(20);
}

int main() {
    stdio_init_all();

    // --- Set up the servo (PWM signal on SERVO_PIN) ---
    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    servo_slice = pwm_gpio_to_slice_num(SERVO_PIN);
    pwm_set_clkdiv(servo_slice, 150.0f);  // Pico runs at 150MHz; /150 makes 1 tick = 1us
    pwm_set_wrap(servo_slice, 20000);     // count up to 20000us (20ms = 50 times a second)
    pwm_set_enabled(servo_slice, true);
    servo_angle(20);                      // start with the gate closed

    // --- Set up the two buttons (a pressed button reads 0) ---
    gpio_init(BTN_INC); gpio_set_dir(BTN_INC, GPIO_IN); gpio_pull_up(BTN_INC);
    gpio_init(BTN_DEC); gpio_set_dir(BTN_DEC, GPIO_IN); gpio_pull_up(BTN_DEC);

    // --- Set up the LCD pins, then the LCD itself ---
    int lcd_pins[6] = {LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7};
    for (int i = 0; i < 6; i++) {
        gpio_init(lcd_pins[i]);
        gpio_set_dir(lcd_pins[i], GPIO_OUT);
    }
    sleep_ms(50);                      // wait for the LCD to power up
    lcd_nibble(0x03); sleep_ms(5);     // standard HD44780 wake-up sequence
    lcd_nibble(0x03); sleep_us(150);
    lcd_nibble(0x03); sleep_us(150);
    lcd_nibble(0x02);                  // switch to 4-bit mode
    lcd_send(0x28, 0);                 // 4-bit mode, 2 lines
    lcd_send(0x0C, 0);                 // display on, no cursor
    lcd_send(0x06, 0);                 // move cursor right after each letter
    lcd_send(0x01, 0); sleep_ms(2);    // clear the screen

    int count = 0;
    show_count(count);
    printf("Parking garage ready.\n");

    // Remember if each button was already held down last time round the loop,
    // so one push only counts once.
    bool inc_held = false;
    bool dec_held = false;

    while (true) {
        bool inc_down = (gpio_get(BTN_INC) == 0);   // true while pressed
        bool dec_down = (gpio_get(BTN_DEC) == 0);

        // "Car in" button just pressed
        if (inc_down && !inc_held) {
            if (count == MAX_CARS) {
                printf("Garage full!\n");
                lcd_print("Parking Lot Full");
                sleep_ms(MESSAGE_MS);
                show_count(count);
            } else {
                count++;
                printf("Car in. Cars: %d\n", count);
                show_count(count);
                run_gate();
            }
        }
        // "Car out" button just pressed
        else if (dec_down && !dec_held) {
            if (count == 0) {
                printf("Garage empty!\n");
                lcd_print("Error");
                sleep_ms(MESSAGE_MS);
                show_count(count);
            } else {
                count--;
                printf("Car out. Cars: %d\n", count);
                show_count(count);
                run_gate();
            }
        }

        inc_held = inc_down;
        dec_held = dec_down;
        sleep_ms(20);   // short pause; also smooths out button bounce
    }
}
