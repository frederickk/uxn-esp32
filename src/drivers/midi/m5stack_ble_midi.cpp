#include <Arduino.h>
#include <BLEMidi.h>

void
midi_ble_init()
{
	BLEMidiServer.begin("uxn-esp32");
}

void
midi_ble_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
	if(BLEMidiServer.isConnected())
		BLEMidiServer.noteOn(channel, note, velocity);
}

void
midi_ble_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
	if(BLEMidiServer.isConnected())
		BLEMidiServer.noteOff(channel, note, velocity);
}
