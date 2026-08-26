#include "config.h"
#ifdef USE_M5STACK_CORE_SPEAKER

#include <Arduino.h>
#include <driver/i2s.h>
extern "C" {
#include <uxn.h>
#include <devices/audio.h>
}

/* M5Stack Core's onboard speaker is driven directly by the ESP32's        */
/* internal 8-bit DAC on GPIO25 (I2S_DAC_CHANNEL_RIGHT_EN). The I2S        */
/* peripheral's built-in-DAC mode is used purely as a convenient DMA-fed   */
/* streaming path to that DAC, not as real I2S output to an external chip. */
/* Samples must be converted from signed PCM to the offset-binary format   */
/* the DAC expects (top 8 bits only) before being written.                 */
/*                                                                          */
/* This DAC is audibly noisy just from being electrically active, even     */
/* while outputting silence — confirmed by disabling the whole subsystem   */
/* and hearing the click disappear entirely, independent of scheduling or  */
/* buffer size. So rather than run it continuously from boot, it's started */
/* only while a channel is actually playing and stopped again once idle.   */

typedef void (*audio_callback_t)(int16_t *, size_t);

#define SAMPLE_FREQUENCY 44100
/* uxn's audio.c divides each channel by 0x180 for mixing headroom, which */
/* leaves plenty of room before this DAC's output stage clips; bump it    */
/* back up here. Raise/lower to taste — clamped below to avoid wraparound. */
#define AUDIO_GAIN 6

static SemaphoreHandle_t mutex;
static audio_callback_t callback;

static const i2s_config_t i2s_config = {
	.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
	.sample_rate = SAMPLE_FREQUENCY,
	.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
	.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
	.communication_format = I2S_COMM_FORMAT_STAND_MSB,
	.intr_alloc_flags = 0,
	.dma_buf_count = 16,
	.dma_buf_len = 64,
	.use_apll = false
};

static void audio_task(void *params);

bool
initaudio(audio_callback_t cb)
{
	callback = cb;

	i2s_driver_uninstall(I2S_NUM_0);

	if(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) {
		Serial.println("error: i2s_driver_install");
		return false;
	}
	if(i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN) != ESP_OK) {
		Serial.println("error: i2s_set_dac_mode");
		return false;
	}
	i2s_stop(I2S_NUM_0);

	mutex = xSemaphoreCreateMutex();
	/* Pinned to the same core as the Arduino loop task (1), away from the  */
	/* NimBLE stack (which runs on core 0) — otherwise BLE radio activity   */
	/* can starve this task long enough to underrun the I2S DMA buffer and  */
	/* produce an audible click, even when uxn isn't actively playing audio. */
	xTaskCreatePinnedToCore(audio_task, "audio_task", 4000, NULL, 10, nullptr, 1);

	return true;
}

void
audio_lock()
{
	xSemaphoreTake(mutex, portMAX_DELAY);
}

void
audio_unlock()
{
	xSemaphoreGive(mutex);
}

/* Keep the DAC engaged for a short while after the last active sample, so */
/* a run of closely-spaced notes (e.g. a fast arpeggio) doesn't repeatedly */
/* stop/start it — each restart has its own brief click too. */
#define IDLE_HANGOVER_MS 200

static bool
any_channel_active()
{
	int i;
	for(i = 0; i < POLYPHONY; i++)
		if(uxn_audio[i].advance) return true;
	return false;
}

static void
audio_task(void *params)
{
	const int samples = 64;
	int16_t stereo_buffer[samples * 2];
	uint16_t mono_buffer[samples];
	size_t bytes_written = 0;
	int i;
	/* uxn ROMs commonly use a zero-length ADSR attack for percussive/plucky */
	/* sounds, which makes audio_render() jump straight from silence to     */
	/* near-full amplitude on the very first sample of a note. On a proper  */
	/* DAC that transient is usually smoothed by analog output filtering;   */
	/* this board's bare internal DAC has none, so it comes through as an   */
	/* audible click. A short one-pole smoothing filter softens any abrupt  */
	/* jump (not just this one case) without altering uxn's audio semantics. */
	static int32_t lp_state = 0;
	bool dac_running = false;
	unsigned long last_active_ms = 0;
	(void)params;

	for(;;) {
		if(any_channel_active()) {
			last_active_ms = millis();
			if(!dac_running) {
				i2s_start(I2S_NUM_0);
				dac_running = true;
			}
		} else if(dac_running && millis() - last_active_ms > IDLE_HANGOVER_MS) {
			i2s_zero_dma_buffer(I2S_NUM_0);
			i2s_stop(I2S_NUM_0);
			dac_running = false;
			lp_state = 0;
		}

		if(!dac_running) {
			vTaskDelay(pdMS_TO_TICKS(5));
			continue;
		}

		xSemaphoreTake(mutex, portMAX_DELAY);
		callback(stereo_buffer, sizeof(stereo_buffer));
		xSemaphoreGive(mutex);

		for(i = 0; i < samples; i++) {
			int32_t mixed = ((int32_t)stereo_buffer[2 * i] + stereo_buffer[2 * i + 1]) * AUDIO_GAIN;
			if(mixed > 32767) mixed = 32767;
			if(mixed < -32768) mixed = -32768;
			lp_state += (mixed - lp_state) >> 3;
			mono_buffer[i] = (uint16_t)((int16_t)lp_state) ^ 0x8000;
		}

		i2s_write(I2S_NUM_0, mono_buffer, sizeof(mono_buffer), &bytes_written, portMAX_DELAY);
	}
}

#endif
