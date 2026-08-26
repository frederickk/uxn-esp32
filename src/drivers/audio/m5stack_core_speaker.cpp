#include "config.h"
#ifdef USE_M5STACK_CORE_SPEAKER

#include <Arduino.h>
#include <driver/i2s.h>

/* M5Stack Core's onboard speaker is driven directly by the ESP32's        */
/* internal 8-bit DAC on GPIO25 (I2S_DAC_CHANNEL_RIGHT_EN). The I2S        */
/* peripheral's built-in-DAC mode is used purely as a convenient DMA-fed   */
/* streaming path to that DAC, not as real I2S output to an external chip. */
/* Samples must be converted from signed PCM to the offset-binary format   */
/* the DAC expects (top 8 bits only) before being written.                 */

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
	.dma_buf_count = 8,
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

	mutex = xSemaphoreCreateMutex();
	xTaskCreate(audio_task, "audio_task", 4000, NULL, 10, nullptr);

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

static void
audio_task(void *params)
{
	const int samples = 64;
	int16_t stereo_buffer[samples * 2];
	uint16_t mono_buffer[samples];
	size_t bytes_written = 0;
	int i;
	(void)params;

	for(;;) {
		xSemaphoreTake(mutex, portMAX_DELAY);
		callback(stereo_buffer, sizeof(stereo_buffer));
		xSemaphoreGive(mutex);

		for(i = 0; i < samples; i++) {
			int32_t mixed = ((int32_t)stereo_buffer[2 * i] + stereo_buffer[2 * i + 1]) * AUDIO_GAIN;
			if(mixed > 32767) mixed = 32767;
			if(mixed < -32768) mixed = -32768;
			mono_buffer[i] = (uint16_t)((int16_t)mixed) ^ 0x8000;
		}

		i2s_write(I2S_NUM_0, mono_buffer, sizeof(mono_buffer), &bytes_written, portMAX_DELAY);
	}
}

#endif
