#include <Arduino.h>
#include <MIDIUSB.h>

/* Relays raw MIDI bytes received on Serial1 (from the M5Stack's GPIO17/    */
/* UART2) into real class-compliant USB-MIDI via the MIDIUSB library. The   */
/* M5Stack always sends complete, back-to-back 3-byte note on/off messages, */
/* so the parser here just resyncs to the next status byte (high bit set)   */
/* and forwards each complete message — this makes it self-healing if a    */
/* byte is ever dropped, without needing anything fancier for this use.    */

#define MIDI_BAUD 31250

static uint8_t buf[3];
static uint8_t idx = 0;

void
setup()
{
	Serial1.begin(MIDI_BAUD);
}

void
loop()
{
	while(Serial1.available()) {
		uint8_t b = Serial1.read();

		if(b & 0x80) {
			buf[0] = b;
			idx = 1;
		} else if(idx > 0 && idx < 3) {
			buf[idx++] = b;
		}

		if(idx == 3) {
			midiEventPacket_t event = {(uint8_t)(buf[0] >> 4), buf[0], buf[1], buf[2]};
			MidiUSB.sendMIDI(event);
			MidiUSB.flush();
			idx = 0;
		}
	}
}
