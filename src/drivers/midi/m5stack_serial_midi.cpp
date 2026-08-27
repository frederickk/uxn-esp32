#include <Arduino.h>

/* Sends raw MIDI 1.0 bytes over UART2 to an external ATmega32U4 board     */
/* (Arduino Micro/Leonardo-class), which relays them to the USB host as a  */
/* real class-compliant USB-MIDI device via the MIDIUSB library — see      */
/* midi-bridge/. Used because the original ESP32 in this board has no      */
/* native USB device controller and can't present as USB-MIDI itself.     */
/*                                                                          */
/* One-way link only (ESP32 -> ATmega): the Micro/Leonardo's TX is 5V      */
/* logic, which isn't safe to feed directly into the ESP32's 3.3V-only RX, */
/* and MIDI output doesn't need anything to come back the other way.       */

#define MIDI_BAUD    31250 /* the actual MIDI 1.0 DIN spec baud rate */
#define MIDI_TX_PIN  17
#define MIDI_RX_PIN  16 /* unused (no wire on this pin), but Serial2.begin() requires a value */

void
midi_serial_init()
{
	Serial2.begin(MIDI_BAUD, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
}

static void
send_message(uint8_t status, uint8_t note, uint8_t velocity)
{
	Serial2.write(status);
	Serial2.write(note & 0x7f);
	Serial2.write(velocity & 0x7f);
}

void
midi_serial_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
	send_message(0x90 | (channel & 0x0f), note, velocity);
}

void
midi_serial_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
	send_message(0x80 | (channel & 0x0f), note, velocity);
}
