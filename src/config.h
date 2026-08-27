#ifndef CONFIG_H
#define CONFIG_H

//#define USE_WIFI

//#define USE_ESP32_DEV_MODULE_BOOT
//#define USE_M5CORE2_BOOT
#define USE_M5STACK_CORE_BOOT

/********* devscreen *********/
//#define USE_TFT_ESPI
//#define USE_M5CORE2_SCREEN
#define USE_M5STACK_CORE_SCREEN
/*****************************/

/********** devctrl **********/
//#define USE_NUNCHUCK
//#define USE_SERIAL2
//#define USE_M5CORE2_BUTTONS
#define USE_M5FACES_KEYBOARD
/*****************************/

/********** devmouse *********/
//#define USE_TOUCH_SCREEN
//#define CAL_DATA { 336, 3549, 262, 3517, 1 }; /* Use Touch_calibrate example of TFT_eSPI lib to calibrate the touch screen */
//#define USE_M5CORE2_TOUCH
#define USE_NIL_MOUSE
/*****************************/

/********** devaudio *********/
#define USE_M5STACK_CORE_SPEAKER
/*****************************/

/********** devmidi **********/
/* Both may be enabled together — every note goes out over whichever of  */
/* these are active. Nothing connected on one just receives nothing.     */
#define USE_M5STACK_BLE_MIDI
#define USE_M5STACK_SERIAL_MIDI
/*****************************/

#endif