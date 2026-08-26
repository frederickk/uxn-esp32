#include "config.h"
#ifdef USE_M5STACK_CORE_SCREEN

#include <M5Stack.h>
extern "C" {
    #include <uxn.h>
    #include <devices/screen.h>
}

extern void error(const char *msg, const char *err);

/* M5Stack's bundled TFT_eSprite has no indexed-colour/palette support (only */
/* 1/8/16 bpp), so the packed 4bpp uxn_screen buffer is converted to RGB565 */
/* by hand and pushed via pushImage() in row-chunks: a full-frame buffer    */
/* doesn't fit in DRAM, and many single-row pushColors() calls inside one   */
/* shared setWindow() proved unreliable on this board's display driver.    */
#define M5_CHUNK_ROWS 40

static Uint16 palette[16];
static Uint16 framebuf[320 * M5_CHUNK_ROWS];

int
devscreen_init() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(TFT_GREEN);
  M5.Lcd.setCursor(0, 0);
  screen_resize(&uxn_screen, M5.Lcd.width(), M5.Lcd.height());
  if(uxn_screen.pixels == NULL)
    error("devscreen_init", "not enough memory");
  return 1;
}

int
devscreen_redraw() {
  Uint32 x, y, w = uxn_screen.width, h = uxn_screen.height, row0;
  Uint8 *pixels = uxn_screen.pixels, p;
  int i;

  for(i = 0; i < 16; i++) {
    Uint16 c = uxn_screen.palette[(i >> 2) ? (i >> 2) : (i & 3)];
    palette[i] = c;
  }

  for(row0 = 0; row0 < h; row0 += M5_CHUNK_ROWS) {
    Uint32 rows = (h - row0 < M5_CHUNK_ROWS) ? (h - row0) : M5_CHUNK_ROWS, fi = 0, ry;
    for(ry = 0; ry < rows; ry++) {
      y = row0 + ry;
      for(x = 0; x < w; x += 2) {
        p = pixels[(y * w + x) / 2];
        framebuf[fi++] = palette[p >> 4];
        framebuf[fi++] = palette[p & 0xf];
      }
    }
    M5.Lcd.pushImage(0, row0, w, rows, framebuf);
  }

  uxn_screen.changed = 0;
  return 1;
}

#endif
