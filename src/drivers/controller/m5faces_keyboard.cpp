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
    if(digitalRead(KEYBOARD_PIN) == LOW) {
        Wire.requestFrom(KEYBOARD_I2C_ADDR, 1);
        while(Wire.available()) {
            Uint8 key = Wire.read();
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
                default:
                    controller_key(d, key);
                    break;
            }
        }
    }
    return 1;
}

#endif
