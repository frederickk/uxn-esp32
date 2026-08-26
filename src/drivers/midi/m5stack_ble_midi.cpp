#include "config.h"
#ifdef USE_M5STACK_BLE_MIDI

#include <Arduino.h>
#include <BLEMidi.h>

void
midi_init()
{
	BLEMidiServer.begin("uxn-esp32");
}

void
midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
	if(BLEMidiServer.isConnected())
		BLEMidiServer.noteOn(channel, note, velocity);
}

void
midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
	if(BLEMidiServer.isConnected())
		BLEMidiServer.noteOff(channel, note, velocity);
}

#endif
