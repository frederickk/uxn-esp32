#include <M5Unified.h>
#include <SPIFFS.h>
#include <Wire.h>

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

static char *rom = "/spiffs/orca.rom";
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
	M5.begin();
	M5.Lcd.setRotation(1);
	M5.Lcd.fillScreen(TFT_BLACK);

	controller_init();
	midi_ble_init();
	midi_serial_init();

	if((ram = (uint8_t *)calloc(0x10000, 1)) == nullptr) {
		Serial.println("Not enough memory for uxn RAM");
		return;
	}
	if((screen_layers = (uint8_t *)calloc((size_t)SCREEN_W * SCREEN_H, 1)) == nullptr) {
		Serial.println("Not enough memory for screen buffer");
		return;
	}

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

	/* Forward bytes typed into the serial monitor into the console
	 * device, the same way uxncli/uxnemu feed stdin -- lets you type a
	 * filename in by hand instead of only relying on open_on_boot. */
	while(Serial.available() > 0) {
		dev[0x12] = Serial.read();
		if(console_vector) uxn_eval(console_vector);
	}

	if(now < next_frame) return;
	next_frame = now + 16; /* 60fps */

	controller_poll();

	if(screen_vector) uxn_eval(screen_vector);
	if(screen_reqdraw) screen_flush();
}
