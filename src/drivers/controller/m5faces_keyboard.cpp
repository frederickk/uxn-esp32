#include "config.h"
#ifdef USE_M5FACES_KEYBOARD

#include <M5Stack.h>
#include <Wire.h>
extern "C" {
#include <uxn.h>
#include <devices/controller.h>
}

#define KEYBOARD_I2C_ADDR 0x08
#define KEYBOARD_PIN      5

#define KEY_UP    0xB7
#define KEY_DOWN  0xC0
#define KEY_LEFT  0xBF
#define KEY_RIGHT 0xC1
#define KEY_ALT   0x9B

/* Standard Varvara controller button bits. */
#define CTRL_BIT 0x01

/* The Faces keyboard reports only one key at a time -- no real Ctrl+key   */
/* chord is possible at the hardware level (confirmed: holding Alt while   */
/* pressing another key still only ever reports one code, and the same    */
/* code repeats on every poll for as long as a key is held). So Alt is a  */
/* sticky software toggle instead, like Caps Lock: press it once to arm   */
/* Ctrl (stays armed indefinitely), press any other key to send it as a   */
/* Ctrl+key pulse and auto-disarm, or press Alt again to cancel without   */
/* sending anything. last_key tracks the previous poll's key (reset to 0  */
/* whenever nothing is pressed) purely so a held Alt is only counted once  */
/* -- otherwise the toggle would flip on every repeated poll while held.  */

int
devctrl_init()
{
    Wire.begin();
    pinMode(KEYBOARD_PIN, INPUT_PULLUP);
    return 1;
}

int
devctrl_handle(Device *d)
{
    static bool ctrl_armed = false;
    static Uint8 last_key = 0;

    if(digitalRead(KEYBOARD_PIN) == LOW) {
        Wire.requestFrom(KEYBOARD_I2C_ADDR, 1);
        while(Wire.available()) {
            Uint8 key = Wire.read();
            bool is_new_press = (key != last_key);
            last_key = key;

            switch(key) {
                case KEY_UP:
                    controller_down(d, 0x10);
                    controller_up(d, 0x10);
                    break;
                case KEY_DOWN:
                    controller_down(d, 0x20);
                    controller_up(d, 0x20);
                    break;
                case KEY_LEFT:
                    controller_down(d, 0x40);
                    controller_up(d, 0x40);
                    break;
                case KEY_RIGHT:
                    controller_down(d, 0x80);
                    controller_up(d, 0x80);
                    break;
                case KEY_ALT:
                    if(is_new_press) {
                        if(ctrl_armed) {
                            controller_up(d, CTRL_BIT);
                            ctrl_armed = false;
                        } else {
                            controller_down(d, CTRL_BIT);
                            ctrl_armed = true;
                        }
                    }
                    break;
                default:
                    controller_key(d, key);
                    if(ctrl_armed) {
                        controller_up(d, CTRL_BIT);
                        ctrl_armed = false;
                    }
                    break;
            }
        }
    } else {
        last_key = 0; /* idle: next press should be detected as new, not a repeat */
    }
    return 1;
}

#endif
