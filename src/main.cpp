#include <M5Unified.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <driver/i2s.h>

/*
Copyright (c) 2021 Devine Lu Linvega

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.

Core interpreter and device dispatch pattern adapted from
git.sr.ht/~rabbits/uxn-m5 (src/uxn-m5.c), the current M5-targeted Uxn
port -- ported here from M5StickC (240x135) to M5Stack Core (320x240),
with the Faces I2C keyboard in place of the two-button demo controller.
*/

static char *rom = "/orca.rom";
static char *open_on_boot = "untitled.orca";

#define SCREEN_W 320
#define SCREEN_H 240

/* ram and screen_layers are heap-allocated (see setup()) rather than    */
/* static arrays -- at 64KB and 75KB respectively (320x240, one byte per */
/* pixel) they don't fit in the fixed static BSS segment together with   */
/* everything else, even though the same total is available on the heap. */
static uint8_t *ram;
static uint8_t dev[0x100], stk[2][0x100], ptr[2];
static uint16_t screen_palette[16];
static unsigned int uxn_eval(uint16_t pc);
static void screen_change(int x1, int y1, int x2, int y2);

static inline uint16_t peek2(const uint8_t *d)
{
	return ((uint16_t)d[0] << 8) | d[1];
}

static inline void poke2(uint8_t *d, uint16_t v)
{
	d[0] = v >> 8;
	d[1] = v;
}

/*
@|System ------------------------------------------------------------ */

static void system_deo_colorize(void)
{
	unsigned int i, shift;
	uint16_t colors[4];
	for(i = 0, shift = 4; i < 4; ++i, shift ^= 4) {
		uint8_t r4 = (dev[0x8 + i/2] >> shift) & 0xf, r8 = (r4 << 4) | r4;
		uint8_t g4 = (dev[0xa + i/2] >> shift) & 0xf, g8 = (g4 << 4) | g4;
		uint8_t b4 = (dev[0xc + i/2] >> shift) & 0xf, b8 = (b4 << 4) | b4;
		colors[i] = ((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3);
	}
	for(i = 0; i < 16; i++)
		screen_palette[i] = colors[i >> 2 ? i >> 2 : i & 3];
	screen_change(0, 0, SCREEN_W, SCREEN_H);
}

/*
@|Console ------------------------------------------------------------- */

/* MIDI is not a dedicated device in the current Uxn spec -- Orca (and the
 * reference `shim` relay it was written against) sends raw MIDI 1.0 bytes
 * out through Console/write (port 0x18) instead. shim resyncs whenever a
 * byte has its high bit set (a MIDI status byte can never appear as a data
 * byte) and dispatches every time it has collected 3 bytes since the last
 * resync. We replicate that exact framing here, then hand the decoded
 * message to the same BLE/serial-bridge transports the old device-based
 * MIDI port used. */
void midi_ble_init();
void midi_ble_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_ble_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_serial_init();
void midi_serial_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_serial_note_off(uint8_t channel, uint8_t note, uint8_t velocity);

static int console_vector = 0;

static void console_deo_write(uint8_t c)
{
	static uint8_t msg[3];
	static int i = 0;
	if(c & 0x80) i = 0;
	msg[i % 3] = c;
	i++;
	if(i % 3 != 0) return;

	const uint8_t status = msg[0], data1 = msg[1], data2 = msg[2];
	const uint8_t channel = status & 0x0f;
	switch(status & 0xf0) {
	case 0x90:
		if(data2 > 0) {
			midi_ble_note_on(channel, data1, data2);
			midi_serial_note_on(channel, data1, data2);
		} else {
			midi_ble_note_off(channel, data1, 0);
			midi_serial_note_off(channel, data1, 0);
		}
		break;
	case 0x80:
		midi_ble_note_off(channel, data1, data2);
		midi_serial_note_off(channel, data1, data2);
		break;
	}
}

/* Types a string into the console device on boot, exactly like a second
 * argv entry to uxncli/uxnemu (see uxn/src/uxncli.c's main()) -- ROMs
 * like Orca that accept a file to open on the command line read it this
 * way rather than via any device or keyboard shortcut. */
static void console_type(const char *s)
{
	if(!*s) return;
	while(*s) {
		dev[0x12] = *s++;
		if(console_vector) uxn_eval(console_vector);
	}
	dev[0x12] = '\n';
	if(console_vector) uxn_eval(console_vector);
}

/*
@|Screen ------------------------------------------------------------ */

static int screen_vector = 0;
static int rX, rY, rA, rMX, rMY, rMA, rML, rDX, rDY;
static int screen_x1, screen_y1, screen_x2, screen_y2, screen_reqdraw;
static uint8_t *screen_layers;
static const uint8_t alpha_lut[16] = {0,1,2,3,4,0,1,2,3,4,0,1,2,3,4,0};
static const uint8_t blend_lut[16][2][4] = {
	{{0,0,1,2},{0,0,4,8}},  {{0,1,2,3},{0,4,8,12}},
	{{0,2,3,1},{0,8,12,4}}, {{0,3,1,2},{0,12,4,8}},
	{{1,0,1,2},{4,0,4,8}},  {{1,1,2,3},{4,4,8,12}},
	{{1,2,3,1},{4,8,12,4}}, {{1,3,1,2},{4,12,4,8}},
	{{2,0,1,2},{8,0,4,8}},  {{2,1,2,3},{8,4,8,12}},
	{{2,2,3,1},{8,8,12,4}}, {{2,3,1,2},{8,12,4,8}},
	{{3,0,1,2},{12,0,4,8}}, {{3,1,2,3},{12,4,8,12}},
	{{3,2,3,1},{12,8,12,4}}, {{3,3,1,2},{12,12,4,8}}
};

static void screen_change(int x1, int y1, int x2, int y2)
{
	if(!screen_reqdraw) { screen_x1=x1; screen_y1=y1; screen_x2=x2; screen_y2=y2; screen_reqdraw=1; return; }
	if(x1 < screen_x1) screen_x1 = x1;
	if(y1 < screen_y1) screen_y1 = y1;
	if(x2 > screen_x2) screen_x2 = x2;
	if(y2 > screen_y2) screen_y2 = y2;
}

static void screen_deo_pixel(void)
{
	const int ctrl = dev[0x2e];
	const int hi = ctrl & 0x40;
	const uint8_t mask = hi ? 0x03 : 0x0c;
	const uint8_t color = hi ? ((ctrl & 0x3) << 2) : (ctrl & 0x3);
	if(ctrl & 0x80) {
		int px, py;
		const int x1 = (ctrl & 0x10) ? 0 : rX;
		const int x2 = (ctrl & 0x10) ? rX : SCREEN_W;
		const int y1 = (ctrl & 0x20) ? 0 : rY;
		const int y2 = (ctrl & 0x20) ? rY : SCREEN_H;
		for(py = y1; py < y2; py++)
			for(px = x1; px < x2; px++) {
				uint8_t *d = &screen_layers[py * SCREEN_W + px];
				*d = (*d & mask) | color;
			}
		screen_change(x1, y1, x2, y2);
	} else {
		const int x = rX, y = rY;
		if(x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
			uint8_t *d = &screen_layers[y * SCREEN_W + x];
			*d = (*d & mask) | color;
			screen_change(x, y, x+1, y+1);
		}
		if(rMX) rX++;
		if(rMY) rY++;
	}
}

static void screen_deo_sprite(void)
{
	int i, j, x = rX, y = rY;
	const int ctrl = dev[0x2f];
	const int flipx = ctrl & 0x10, dx = flipx ? -rDY : rDY;
	const int flipy = ctrl & 0x20, dy = flipy ? -rDX : rDX;
	const int row_start = flipx ? 0 : 7, row_delta = flipx ? 1 : -1;
	const int col_start = flipy ? 7 : 0, col_delta = flipy ? -1 : 1;
	const int layer = ctrl & 0x40, layer_mask = layer ? 0x3 : 0xc;
	const int mode_2bpp = ctrl >> 7, addr_2bpp = mode_2bpp ? rMA << 2 : rMA << 1;
	const int blend = ctrl & 0xf;
	const uint8_t opaque = alpha_lut[blend];
	const uint8_t *table = blend_lut[blend][layer >> 6];
	for(i = 0; i <= rML; i++, x += dx, y += dy, rA += addr_2bpp) {
		if(x < -8 || x >= SCREEN_W || y < -8 || y >= SCREEN_H) continue;
		const uint8_t *col = &ram[rA + col_start];
		for(j = 0; j < 8; j++, col += col_delta) {
			const int ch1 = *col, ch2 = mode_2bpp ? col[8] : 0;
			for(int k = 0, row = row_start; k < 8; k++, row += row_delta) {
				int px = x + k, py = y + j;
				if(px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
				const int color = ((ch1 >> row) & 1) | (((ch2 >> row) & 1) << 1);
				if(opaque || color) {
					uint8_t *d = &screen_layers[py * SCREEN_W + px];
					*d = (*d & layer_mask) | table[color];
				}
			}
		}
	}
	{
		int x1 = flipx ? x : rX, x2 = flipx ? rX : x;
		int y1 = flipy ? y : rY, y2 = flipy ? rY : y;
		screen_change(x1-8, y1-8, x2+8, y2+8);
	}
	if(rMX) rX += flipx ? -rDX : rDX;
	if(rMY) rY += flipy ? -rDY : rDY;
}

static void screen_flush(void)
{
	int x1 = max(0, screen_x1), y1 = max(0, screen_y1);
	int x2 = min(SCREEN_W, screen_x2), y2 = min(SCREEN_H, screen_y2);
	int w = x2 - x1, h = y2 - y1;
	if(w <= 0 || h <= 0) return;
	static uint16_t row[SCREEN_W];
	M5.Lcd.startWrite();
	M5.Lcd.setAddrWindow(x1, y1, w, h);
	for(int y = y1; y < y2; y++) {
		for(int x = x1; x < x2; x++)
			row[x - x1] = screen_palette[screen_layers[y * SCREEN_W + x] & 0xf];
		M5.Lcd.pushColors(row, w, true);
	}
	M5.Lcd.endWrite();
	screen_x1 = screen_y1 = screen_x2 = screen_y2 = screen_reqdraw = 0;
}

static void screen_deo_auto(void)
{
	rMX = dev[0x26] & 0x1;
	rMY = dev[0x26] & 0x2;
	rMA = dev[0x26] & 0x4;
	rML = dev[0x26] >> 4;
	rDX = rMX << 3;
	rDY = rMY << 2;
}

/*
@|Audio ---------------------------------------------------------------- */

/* Ported from the reference Audio device (git.sr.ht/~rabbits/uxn2's
 * uxn2.c) -- four independent ADSR-enveloped sample-playback channels
 * at 0x30/0x40/0x50/0x60, 0x10 bytes apart. The physics (envelope, pitch
 * table, mixing) is unchanged from the version already proven out on
 * this board in the old Device-API branch, including the audible-click
 * fix below; only the dispatch glue changes to match this file's
 * single-function emu_dei/emu_deo style and the current port addresses. */

#define AUDIO_POLYPHONY 4
#define AUDIO_SAMPLE_FREQUENCY 44100
#define AUDIO_NOTE_PERIOD (AUDIO_SAMPLE_FREQUENCY * 0x4000 / 11025)
#define AUDIO_ADSR_STEP (AUDIO_SAMPLE_FREQUENCY / 0xf)
/* uxn's mixer divides each channel by 0x180 for headroom; this bare DAC
 * has plenty of room before its output stage clips, so make up for it
 * here. Raise/lower to taste -- clamped below to avoid wraparound. */
#define AUDIO_GAIN 6

typedef struct {
	uint8_t *addr;
	uint32_t count, advance, period, age, a, d, s, r;
	uint16_t i, len;
	int8_t volume[2];
	uint8_t pitch, repeat;
} UxnAudio;

static const uint32_t audio_advances[12] = {
	0x80000, 0x879c8, 0x8facd, 0x9837f, 0xa1451, 0xaadc1,
	0xb504f, 0xbfc88, 0xcb2ff, 0xd7450, 0xe411f, 0xf1a1c
};

static UxnAudio uxn_audio[AUDIO_POLYPHONY];
static int audio_vector[AUDIO_POLYPHONY];
static volatile uint8_t audio_finished_flags = 0;
static SemaphoreHandle_t audio_mutex;

static int32_t audio_envelope(UxnAudio *c, uint32_t age)
{
	if(!c->r) return 0x0888;
	if(age < c->a) return 0x0888 * age / c->a;
	if(age < c->d) return 0x0444 * (2 * c->d - c->a - age) / (c->d - c->a);
	if(age < c->s) return 0x0444;
	if(age < c->r) return 0x0444 * (c->r - age) / (c->r - c->s);
	c->advance = 0;
	return 0x0000;
}

static int audio_render(int instance, int16_t *sample, int16_t *end)
{
	UxnAudio *c = &uxn_audio[instance];
	int32_t s;
	if(!c->advance || !c->period) return 0;
	while(sample < end) {
		c->count += c->advance;
		c->i += c->count / c->period;
		c->count %= c->period;
		if(c->i >= c->len) {
			if(!c->repeat) { c->advance = 0; break; }
			c->i %= c->len;
		}
		s = (int8_t)(c->addr[c->i] + 0x80) * audio_envelope(c, c->age++);
		*sample++ += s * c->volume[0] / 0x180;
		*sample++ += s * c->volume[1] / 0x180;
	}
	if(!c->advance) audio_finished_flags |= (1 << instance);
	return 1;
}

static void audio_start(int instance, uint8_t *d)
{
	UxnAudio *c = &uxn_audio[instance];
	uint8_t pitch = d[0xf] & 0x7f;
	uint16_t addr = peek2(&d[0xc]);
	uint16_t adsr = peek2(&d[0x8]);
	c->len = peek2(&d[0xa]);
	if(c->len > 0x10000 - addr) c->len = 0x10000 - addr;
	c->addr = &ram[addr];
	c->volume[0] = d[0xe] >> 4;
	c->volume[1] = d[0xe] & 0xf;
	c->repeat = !(d[0xf] & 0x80);
	if(pitch < 108 && c->len)
		c->advance = audio_advances[pitch % 12] >> (8 - pitch / 12);
	else {
		c->advance = 0;
		return;
	}
	c->a = AUDIO_ADSR_STEP * (adsr >> 12);
	c->d = AUDIO_ADSR_STEP * (adsr >> 8 & 0xf) + c->a;
	c->s = AUDIO_ADSR_STEP * (adsr >> 4 & 0xf) + c->d;
	c->r = AUDIO_ADSR_STEP * (adsr >> 0 & 0xf) + c->s;
	c->age = 0;
	c->i = 0;
	if(c->len <= 0x100) /* single cycle mode */
		c->period = AUDIO_NOTE_PERIOD * 337 / 2 / c->len;
	else /* sample repeat mode */
		c->period = AUDIO_NOTE_PERIOD;
}

static void audio_play(int instance)
{
	xSemaphoreTake(audio_mutex, portMAX_DELAY);
	audio_start(instance, &dev[0x30 + instance * 0x10]);
	xSemaphoreGive(audio_mutex);
}

static uint8_t audio_get_vu(UxnAudio *c)
{
	int i;
	int32_t sum[2] = {0, 0};
	if(!c->advance || !c->period) return 0;
	for(i = 0; i < 2; i++) {
		if(!c->volume[i]) continue;
		sum[i] = 1 + audio_envelope(c, c->age) * c->volume[i] / 0x800;
		if(sum[i] > 0xf) sum[i] = 0xf;
	}
	return (sum[0] << 4) | sum[1];
}

static uint8_t audio_dei(uint8_t port)
{
	UxnAudio *c = &uxn_audio[(port >> 4) - 3];
	switch(port & 0xf) {
	case 0x2: return c->i >> 8;
	case 0x3: return c->i;
	case 0x4: return audio_get_vu(c);
	default: return dev[port];
	}
}

static void audio_deo(uint8_t port)
{
	int instance = (port >> 4) - 3;
	switch(port & 0xf) {
	case 0x1: audio_vector[instance] = peek2(&dev[port & 0xf0]); break;
	case 0xf: audio_play(instance); break;
	}
}

/* Keep the DAC engaged for a short while after the last active sample, so
 * a run of closely-spaced notes (e.g. a fast arpeggio) doesn't repeatedly
 * stop/start it -- each restart has its own brief click too. */
#define AUDIO_IDLE_HANGOVER_MS 200

static bool audio_any_channel_active(void)
{
	int i;
	for(i = 0; i < AUDIO_POLYPHONY; i++)
		if(uxn_audio[i].advance) return true;
	return false;
}

static void audio_mix(int16_t *stream, size_t bytes)
{
	memset(stream, 0, bytes);
	for(int i = 0; i < AUDIO_POLYPHONY; i++)
		audio_render(i, stream, stream + bytes / sizeof(int16_t));
}

static const i2s_config_t audio_i2s_config = {
	.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
	.sample_rate = AUDIO_SAMPLE_FREQUENCY,
	.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
	.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
	.communication_format = I2S_COMM_FORMAT_STAND_MSB,
	.intr_alloc_flags = 0,
	.dma_buf_count = 16,
	.dma_buf_len = 64,
	.use_apll = false
};

static void audio_task(void *params)
{
	const int samples = 64;
	int16_t stereo_buffer[samples * 2];
	uint16_t mono_buffer[samples];
	size_t bytes_written = 0;
	int i;
	/* uxn ROMs commonly use a zero-length ADSR attack for percussive/plucky
	 * sounds, which makes audio_render() jump straight from silence to
	 * near-full amplitude on the very first sample of a note. This bare
	 * internal DAC has no analog output filtering to smooth that transient,
	 * so it comes through as an audible click; a one-pole smoothing filter
	 * softens any abrupt jump (not just this one case) without altering
	 * uxn's audio semantics. */
	static int32_t lp_state = 0;
	bool dac_running = false;
	unsigned long last_active_ms = 0;
	(void)params;

	for(;;) {
		if(audio_any_channel_active()) {
			last_active_ms = millis();
			if(!dac_running) { i2s_start(I2S_NUM_0); dac_running = true; }
		} else if(dac_running && millis() - last_active_ms > AUDIO_IDLE_HANGOVER_MS) {
			i2s_zero_dma_buffer(I2S_NUM_0);
			i2s_stop(I2S_NUM_0);
			dac_running = false;
			lp_state = 0;
		}

		if(!dac_running) {
			vTaskDelay(pdMS_TO_TICKS(5));
			continue;
		}

		xSemaphoreTake(audio_mutex, portMAX_DELAY);
		audio_mix(stereo_buffer, sizeof(stereo_buffer));
		xSemaphoreGive(audio_mutex);

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

/* M5Stack Core's onboard speaker is driven directly by the ESP32's internal
 * 8-bit DAC on GPIO25 (I2S_DAC_CHANNEL_RIGHT_EN); the I2S peripheral's
 * built-in-DAC mode is just a convenient DMA-fed streaming path to it, not
 * real I2S output to an external chip. This DAC is audibly noisy just from
 * being electrically active, even while outputting silence -- confirmed by
 * disabling the whole subsystem and hearing the click disappear entirely,
 * independent of scheduling or buffer size -- so it's started only while a
 * channel is actually playing and stopped again once idle (audio_task()). */
static bool audio_init(void)
{
	i2s_driver_uninstall(I2S_NUM_0);
	if(i2s_driver_install(I2S_NUM_0, &audio_i2s_config, 0, NULL) != ESP_OK) {
		Serial.println("error: i2s_driver_install");
		return false;
	}
	if(i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN) != ESP_OK) {
		Serial.println("error: i2s_set_dac_mode");
		return false;
	}
	i2s_stop(I2S_NUM_0);

	audio_mutex = xSemaphoreCreateMutex();
	/* Pinned to the same core as the Arduino loop task (1), away from the
	 * NimBLE stack (core 0) -- otherwise BLE radio activity can starve this
	 * task long enough to underrun the I2S DMA buffer and click, even when
	 * uxn isn't actively playing audio. */
	xTaskCreatePinnedToCore(audio_task, "audio_task", 4000, NULL, 10, nullptr, 1);
	return true;
}

/*
@|Controller (Faces I2C keyboard) ------------------------------------ */

#define KEYBOARD_I2C_ADDR 0x08
#define KEYBOARD_PIN      5

#define KEY_UP    0xB7
#define KEY_DOWN  0xC0
#define KEY_LEFT  0xBF
#define KEY_RIGHT 0xC1
#define KEY_ALT   0x9B
#define CTRL_BIT  0x01

static int controller_vector = 0;

static void controller_down(uint8_t mask)
{
	if(!mask) return;
	dev[0x82] |= mask;
	if(controller_vector) uxn_eval(controller_vector);
}

static void controller_up(uint8_t mask)
{
	if(!mask) return;
	dev[0x82] &= ~mask;
	if(controller_vector) uxn_eval(controller_vector);
}

static void controller_key(uint8_t key)
{
	if(!key) return;
	dev[0x83] = key;
	if(controller_vector) uxn_eval(controller_vector);
	dev[0x83] = 0;
}

static void controller_init(void)
{
	Wire.begin();
	pinMode(KEYBOARD_PIN, INPUT_PULLUP);
}

/* Alt is a sticky software toggle for Ctrl, like Caps Lock: press it once
 * to arm (stays armed indefinitely), press any other key to send it as
 * Ctrl+key and auto-disarm, or press Alt again to cancel. The keyboard
 * only ever reports one key at a time -- no real simultaneous Ctrl+key
 * chord is possible at the hardware level -- so this emulates it here.
 * last_key tracks the previous poll's key (reset to 0 when idle) purely
 * so a held Alt is only counted once, not re-toggled on every repeat. */
static void controller_poll(void)
{
	static bool ctrl_armed = false;
	static uint8_t last_key = 0;

	if(digitalRead(KEYBOARD_PIN) == LOW) {
		Wire.requestFrom(KEYBOARD_I2C_ADDR, 1);
		while(Wire.available()) {
			uint8_t key = Wire.read();
			bool is_new_press = (key != last_key);
			last_key = key;

			switch(key) {
				case KEY_UP:    controller_down(0x10); controller_up(0x10); break;
				case KEY_DOWN:  controller_down(0x20); controller_up(0x20); break;
				case KEY_LEFT:  controller_down(0x40); controller_up(0x40); break;
				case KEY_RIGHT: controller_down(0x80); controller_up(0x80); break;
				case KEY_ALT:
					if(is_new_press) {
						if(ctrl_armed) { controller_up(CTRL_BIT); ctrl_armed = false; }
						else { controller_down(CTRL_BIT); ctrl_armed = true; }
					}
					break;
				default:
					controller_key(key);
					if(ctrl_armed) { controller_up(CTRL_BIT); ctrl_armed = false; }
					break;
			}
		}
	} else {
		last_key = 0;
	}
}

/*
@|File ----------------------------------------------------------------- */

/* SPIFFS is a flat namespace -- no real directories -- so this covers
 * plain read/write/stat/delete-by-name, enough for Orca's open/save
 * workflow (typing a filename into the console), not directory listing. */

static char *file_path = nullptr;
static File file_handle;
static enum { FILE_IDLE, FILE_READING, FILE_WRITING } file_state = FILE_IDLE;
static uint16_t file_length = 0;

static void file_full_path(char *out, size_t outsz, const char *name)
{
	if(name[0] == '/') snprintf(out, outsz, "%s", name);
	else snprintf(out, outsz, "/%s", name);
}

static void file_reset(void)
{
	if(file_state != FILE_IDLE) file_handle.close();
	file_state = FILE_IDLE;
}

static bool file_open(bool want_write, uint8_t append)
{
	char path[256];
	if(!file_path) return false;
	file_full_path(path, sizeof(path), file_path);
	if(want_write) {
		if(file_state == FILE_WRITING) return true;
		file_reset();
		file_handle = SPIFFS.open(path, append ? "a" : "w");
		if(file_handle) file_state = FILE_WRITING;
	} else {
		if(file_state == FILE_READING) return true;
		file_reset();
		file_handle = SPIFFS.open(path, "r");
		if(file_handle) file_state = FILE_READING;
	}
	return file_state != FILE_IDLE;
}

static uint16_t file_init(uint16_t addr)
{
	file_reset();
	file_path = (char *)&ram[addr];
	return 0;
}

static uint16_t file_read(uint16_t addr, uint16_t len)
{
	if(!file_path) return 0;
	if((uint32_t)addr + len > 0x10000) len = 0x10000 - addr;
	if(!file_open(false, 0)) return 0;
	size_t n = file_handle.read(&ram[addr], len);
	if(len > 0 && n == 0) file_reset();
	return n;
}

static uint16_t file_write(uint16_t addr, uint16_t len, uint8_t flags)
{
	if(!file_path) return 0;
	if((uint32_t)addr + len > 0x10000) len = 0x10000 - addr;
	if(!file_open(true, flags & 0x01)) return 0;
	size_t n = file_handle.write(&ram[addr], len);
	file_handle.flush();
	return n;
}

static uint16_t file_stat(uint16_t addr, uint16_t len)
{
	char path[256];
	uint16_t n = len < 4 ? len : 4;
	if(!file_path || len == 0) return 0;
	file_full_path(path, sizeof(path), file_path);
	File f = SPIFFS.open(path, "r");
	if(f) {
		char buf[5];
		snprintf(buf, sizeof(buf), "%04x", (unsigned)(f.size() & 0xffff));
		f.close();
		memcpy(&ram[addr], buf, n);
	} else {
		memset(&ram[addr], '!', n);
	}
	return n;
}

static uint16_t file_delete(void)
{
	char path[256];
	if(!file_path) return 0;
	file_full_path(path, sizeof(path), file_path);
	file_reset();
	return SPIFFS.remove(path) ? 1 : 0;
}

/*
@|DateTime --------------------------------------------------------------- */

static uint8_t datetime_dei(const uint8_t port)
{
	time_t seconds = time(NULL);
	struct tm zt = {0};
	struct tm *t = localtime(&seconds);
	if(t == NULL) t = &zt;
	switch(port) {
	case 0xc0: return (t->tm_year + 1900) >> 8;
	case 0xc1: return (t->tm_year + 1900);
	case 0xc2: return t->tm_mon;
	case 0xc3: return t->tm_mday;
	case 0xc4: return t->tm_hour;
	case 0xc5: return t->tm_min;
	case 0xc6: return t->tm_sec;
	case 0xc7: return t->tm_wday;
	case 0xc8: return t->tm_yday >> 8;
	case 0xc9: return t->tm_yday;
	case 0xca: return t->tm_isdst;
	default: return dev[port];
	}
}

/*
@|Device dispatch ----------------------------------------------------- */

static inline uint8_t emu_dei(const uint8_t port)
{
	switch(port) {
	case 0x22: return SCREEN_W >> 8;
	case 0x23: return SCREEN_W;
	case 0x24: return SCREEN_H >> 8;
	case 0x25: return SCREEN_H;
	case 0x28: return rX >> 8;
	case 0x29: return rX;
	case 0x2a: return rY >> 8;
	case 0x2b: return rY;
	case 0x2c: return rA >> 8;
	case 0x2d: return rA;
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x52:
	case 0x53:
	case 0x54:
	case 0x62:
	case 0x63:
	case 0x64: return audio_dei(port);
	case 0xc0:
	case 0xc1:
	case 0xc2:
	case 0xc3:
	case 0xc4:
	case 0xc5:
	case 0xc6:
	case 0xc7:
	case 0xc8:
	case 0xc9:
	case 0xca: return datetime_dei(port);
	default: return dev[port];
	}
}

static inline void emu_deo(const uint8_t port, const uint8_t value)
{
	dev[port] = value;
	switch(port) {
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0b:
	case 0x0c:
	case 0x0d: system_deo_colorize(); break;
	case 0x11: console_vector = peek2(&dev[0x10]); break;
	case 0x18: console_deo_write(value); break;
	case 0x19: Serial.write(value); break;
	case 0x21: screen_vector = peek2(&dev[0x20]); break;
	case 0x26: screen_deo_auto(); break;
	case 0x28:
	case 0x29: rX = (short)peek2(&dev[0x28]); break;
	case 0x2a:
	case 0x2b: rY = (short)peek2(&dev[0x2a]); break;
	case 0x2c:
	case 0x2d: rA = peek2(&dev[0x2c]); break;
	case 0x2e: screen_deo_pixel(); break;
	case 0x2f: screen_deo_sprite(); break;
	case 0x31:
	case 0x3f:
	case 0x41:
	case 0x4f:
	case 0x51:
	case 0x5f:
	case 0x61:
	case 0x6f: audio_deo(port); break;
	case 0x81: controller_vector = peek2(&dev[0x80]); break;
	case 0xa5: poke2(&dev[0xa2], file_stat(peek2(&dev[0xa4]), file_length)); break;
	case 0xa6: poke2(&dev[0xa2], file_delete()); break;
	case 0xa9: poke2(&dev[0xa2], file_init(peek2(&dev[0xa8]))); break;
	case 0xab: file_length = peek2(&dev[0xaa]); break;
	case 0xad: poke2(&dev[0xa2], file_read(peek2(&dev[0xac]), file_length)); break;
	case 0xaf: poke2(&dev[0xa2], file_write(peek2(&dev[0xae]), file_length, dev[0xa7])); break;
	}
}

/*
@|Core ----------------------------------------------------------------- */

#define OPC(opc, A, B) {\
	case 0x00|opc: {const uint8_t d=0,r=0;A B} break;\
	case 0x20|opc: {const uint8_t d=1,r=0;A B} break;\
	case 0x40|opc: {const uint8_t d=0,r=1;A B} break;\
	case 0x60|opc: {const uint8_t d=1,r=1;A B} break;\
	case 0x80|opc: {const uint8_t d=0,r=0,k=ptr[0];A ptr[0]=k;B} break;\
	case 0xa0|opc: {const uint8_t d=1,r=0,k=ptr[0];A ptr[0]=k;B} break;\
	case 0xc0|opc: {const uint8_t d=0,r=1,k=ptr[1];A ptr[1]=k;B} break;\
	case 0xe0|opc: {const uint8_t d=1,r=1,k=ptr[1];A ptr[1]=k;B} break;}
#define DEC(m) stk[m][--ptr[m]]
#define INC(m) stk[m][ptr[m]++]
#define IMM a = ram[pc++] << 8, a |= ram[pc++];
#define MOV pc = d ? (uint16_t)a : pc + (int8_t)a;
#define POx(o,m) o = DEC(r); if(m) o |= DEC(r) << 8;
#define PUx(i,m,s) if(m) c = (i), INC(s) = c >> 8, INC(s) = c; else INC(s) = i;
#define GOT(o) if(d) o[1] = DEC(r); o[0] = DEC(r);
#define PUT(i,s) INC(s) = i[0]; if(d) INC(s) = i[1];
#define DEO(o,v) emu_deo(o, v[0]); if(d) emu_deo(o + 1, v[1]);
#define DEI(i,v) v[0] = emu_dei(i); if(d) v[1] = emu_dei(i + 1); PUT(v,r)
#define POK(o,v,m) ram[o] = v[0]; if(d) ram[(o + 1) & m] = v[1];
#define PEK(i,v,m) v[0] = ram[i]; if(d) v[1] = ram[(i + 1) & m]; PUT(v,r)

static unsigned int uxn_eval(uint16_t pc)
{
	unsigned int a, b, c;
	uint16_t x[2], y[2], z[2];
	for(;;)
	switch(ram[pc++]) {
	case 0x00: return 1;
	case 0x20: if(DEC(0)) { IMM pc += a; } else pc += 2; break;
	case 0x40: IMM pc += a; break;
	case 0x60: IMM PUx(pc, 1, 1) pc += a; break;
	case 0xa0: INC(0) = ram[pc++]; /* fall-through */
	case 0x80: INC(0) = ram[pc++]; break;
	case 0xe0: INC(1) = ram[pc++]; /* fall-through */
	case 0xc0: INC(1) = ram[pc++]; break;
	OPC(0x01,POx(a,d),PUx(a+1,d,r))
	OPC(0x02,ptr[r] -= 1+d;,{})
	OPC(0x03,GOT(x) ptr[r] -= 1+d;,PUT(x,r))
	OPC(0x04,GOT(x) GOT(y),PUT(x,r) PUT(y,r))
	OPC(0x05,GOT(x) GOT(y) GOT(z),PUT(y,r) PUT(x,r) PUT(z,r))
	OPC(0x06,GOT(x),PUT(x,r) PUT(x,r))
	OPC(0x07,GOT(x) GOT(y),PUT(y,r) PUT(x,r) PUT(y,r))
	OPC(0x08,POx(a,d) POx(b,d),PUx(b==a,0,r))
	OPC(0x09,POx(a,d) POx(b,d),PUx(b!=a,0,r))
	OPC(0x0a,POx(a,d) POx(b,d),PUx(b>a,0,r))
	OPC(0x0b,POx(a,d) POx(b,d),PUx(b<a,0,r))
	OPC(0x0c,POx(a,d),MOV)
	OPC(0x0d,POx(a,d) POx(b,0),if(b) MOV)
	OPC(0x0e,POx(a,d),PUx(pc,1,!r) MOV)
	OPC(0x0f,GOT(x),PUT(x,!r))
	OPC(0x10,POx(a,0),PEK(a,x,0xff))
	OPC(0x11,POx(a,0) GOT(y),POK(a,y,0xff))
	OPC(0x12,POx(a,0),PEK(pc+(int8_t)a,x,0xffff))
	OPC(0x13,POx(a,0) GOT(y),POK(pc+(int8_t)a,y,0xffff))
	OPC(0x14,POx(a,1),PEK(a,x,0xffff))
	OPC(0x15,POx(a,1) GOT(y),POK(a,y,0xffff))
	OPC(0x16,POx(a,0),DEI(a,x))
	OPC(0x17,POx(a,0) GOT(y),DEO(a,y))
	OPC(0x18,POx(a,d) POx(b,d),PUx(b+a,d,r))
	OPC(0x19,POx(a,d) POx(b,d),PUx(b-a,d,r))
	OPC(0x1a,POx(a,d) POx(b,d),PUx(b*a,d,r))
	OPC(0x1b,POx(a,d) POx(b,d),PUx(a?b/a:0,d,r))
	OPC(0x1c,POx(a,d) POx(b,d),PUx(b&a,d,r))
	OPC(0x1d,POx(a,d) POx(b,d),PUx(b|a,d,r))
	OPC(0x1e,POx(a,d) POx(b,d),PUx(b^a,d,r))
	OPC(0x1f,POx(a,0) POx(b,d),PUx(b>>(a&0xf)<<(a>>4),d,r))
	} return 0;
}

/*
@|Boot ------------------------------------------------------------------ */

static bool load_rom(const char *filepath)
{
	File f = SPIFFS.open(filepath, "r");
	if(!f) return false;
	size_t r = f.read(&ram[0x100], 0x10000 - 0x100);
	f.close();
	if(r < 1) return false;
	Serial.printf("Loaded %s (%u bytes)\n", filepath, (unsigned)r);
	return true;
}

void setup(void)
{
	Serial.begin(115200);

	/* Grab the two big VM buffers first, while the heap is still one
	 * contiguous block. M5.begin()/audio_init()/midi_ble_init() (NimBLE
	 * in particular) each carve out their own smaller allocations; by
	 * the time they're done, tens of KB of aggregate free heap can be
	 * fragmented into pieces too small to satisfy a single ~75KB
	 * calloc(), even though the total is more than enough. */
	if((ram = (uint8_t *)calloc(0x10000, 1)) == nullptr) {
		Serial.println("Not enough memory for uxn RAM");
		return;
	}
	if((screen_layers = (uint8_t *)calloc((size_t)SCREEN_W * SCREEN_H, 1)) == nullptr) {
		Serial.println("Not enough memory for screen buffer");
		return;
	}

	M5.begin();
	M5.Lcd.setRotation(1);
	M5.Lcd.fillScreen(TFT_BLACK);

	controller_init();
	audio_init();
	midi_ble_init();
	midi_serial_init();

	if(!SPIFFS.begin()) {
		Serial.println("SPIFFS mount failed");
		return;
	}
	if(!load_rom(rom)) {
		Serial.printf("Failed to load %s\n", rom);
		return;
	}
	uxn_eval(0x100);
	console_type(open_on_boot);
}

void loop(void)
{
	static uint32_t next_frame = 0;
	uint32_t now = millis();

	if(dev[0xf]) return; /* ROM halted -- see System/state in orca.tal's device table */

	/* Forward bytes typed into the serial monitor into the console
	 * device, the same way uxncli/uxnemu feed stdin -- lets you type a
	 * filename in by hand instead of only relying on open_on_boot. */
	while(Serial.available() > 0) {
		dev[0x12] = Serial.read();
		if(console_vector) uxn_eval(console_vector);
	}

	/* Audio channels finish on the audio task's own thread (see
	 * audio_render()), which must not call back into uxn_eval() itself --
	 * that would let the audio task and the main thread mutate uxn's
	 * single-threaded VM state concurrently. It just raises a flag here
	 * instead, polled and evaluated from the main thread like everything
	 * else. */
	if(audio_finished_flags) {
		for(int i = 0; i < AUDIO_POLYPHONY; i++) {
			if(audio_finished_flags & (1 << i)) {
				audio_finished_flags &= ~(1 << i);
				if(audio_vector[i]) uxn_eval(audio_vector[i]);
			}
		}
	}

	if(now < next_frame) return;
	next_frame = now + 16; /* 60fps */

	controller_poll();

	if(screen_vector) uxn_eval(screen_vector);
	if(screen_reqdraw) screen_flush();
}
