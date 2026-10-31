/* Regression: the mosaic table has to exist on a host that never renders line 0.
 *
 * mosaicModulo was built in the line == 0 block of the line job, and frame-model
 * hosts (beam_frame_driver.c) render 1..224. On FF6's first mosaic, the battle
 * transition, the table was still all zeroes: the span width at x came out as
 * size - x, negative from x > size, and the draw loop ran off the line buffer.
 *
 * No game ROM, generated data, or platform frontend is required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

/* ppu.c references these runtime globals, but this harness links ppu.c alone. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t real_tile) {
    (void)layer;
    (void)screen_x;
    (void)wrapped_y;
    return real_tile;
}

bool WsShadowLayerActive(int layer) {
    (void)layer;
    return false;
}

uint32_t WsShadowWorldX(int layer) {
    (void)layer;
    return 0;
}

uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer;
    (void)screen_x;
    return 0;
}

uint32_t WsShadowScrollY(int layer) {
    (void)layer;
    return 0;
}

void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr;
    (void)value;
}

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

/* mosaicModulo[i] is i rounded down to a block of `size`. */
static bool table_is(const Ppu *ppu, int size) {
    for (int i = 0; i < kPpuXPixels; i++) {
        if (ppu->mosaicModulo[i] != (uint8_t)(i - i % size)) return false;
    }
    return true;
}

static void setup_solid_bg1(Ppu *ppu, bool layer_on) {
    ppu_reset(ppu);
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = layer_on ? 1 : 0;
    ppu->bgXsc[0] = 0x00;
    ppu->bgTileAdr = 0x0001;
    memset(ppu->vram, 0, sizeof ppu->vram);
    for (int i = 0; i < 32 * 32; i++) ppu->vram[i] = 1;
    /* Tile 1, 4bpp: colour 1 throughout. */
    for (int row = 0; row < 8; row++) ppu->vram[0x1000 + 16 + row] = 0x00ff;
    memset(ppu->cgram, 0, sizeof ppu->cgram);
    ppu->cgram[1] = 0x001f;  /* red */
}

int main(void) {
    enum { kPitch = kPpuXPixels * 4 };
    static uint8_t pixels[kPitch * 8];
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;

    /* Layer off, so the old table shows up as a wrong table and not as a crash. */
    setup_solid_bg1(ppu, false);
    ppu_write(ppu, 0x06, 0x31);  /* size 4, BG1 */
    PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
    ppu_runLine(ppu, 1);
    failures += check(table_is(ppu, 4), "first rendered line 1 builds the table");

    /* A guest may change the size on any line (FF6 drives $2106 by HDMA). */
    ppu_write(ppu, 0x06, 0x11);
    ppu_runLine(ppu, 2);
    failures += check(table_is(ppu, 2), "a size change between lines rebuilds the table");

    /* The path that crashed: a mosaic BG, drawn without a line 0, whose span
     * starts past the first block. Window 1 hides BG1 on x = 0..15, so the span
     * starts at 16; with an empty table its first width is 4 - 16. */
    setup_solid_bg1(ppu, true);
    ppu_write(ppu, 0x06, 0x31);
    ppu_write(ppu, 0x23, 0x02);  /* W12SEL: window 1 on BG1 */
    ppu_write(ppu, 0x26, 0);     /* WH0 */
    ppu_write(ppu, 0x27, 15);    /* WH1 */
    ppu_write(ppu, 0x2e, 0x01);  /* TMW: BG1 */
    memset(pixels, 0, sizeof pixels);
    PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
    ppu_runLine(ppu, 1);
    bool span_ok = true;
    for (int x = 0; x < 256; x++) {
        const uint8_t *p = pixels + (size_t)x * 4;  /* BGRX */
        const uint8_t want_red = x < 16 ? 0 : 0xff;
        if (p[0] != 0 || p[1] != 0 || p[2] != want_red) span_ok = false;
    }
    failures += check(span_ok, "a mosaic BG behind a window fills its span and only that");

    /* The vertical block counts from the first visible line. Tile 1 has one
     * colour per row, so each drawn line names the row it was sampled from:
     * with size 4, lines 1-4 show row 1 and lines 5-8 row 5. */
    setup_solid_bg1(ppu, true);
    ppu_write(ppu, 0x06, 0x31);
    for (int row = 0; row < 8; row++) {
        const int color = row + 1;
        uint16_t lo = 0, hi = 0;
        if (color & 1) lo |= 0x00ff;
        if (color & 2) lo |= 0xff00;
        if (color & 4) hi |= 0x00ff;
        if (color & 8) hi |= 0xff00;
        ppu->vram[0x1000 + 16 + row] = lo;
        ppu->vram[0x1000 + 16 + 8 + row] = hi;
        ppu->cgram[color] = (uint16_t)(color * 3);
    }
    memset(pixels, 0, sizeof pixels);
    PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
    bool blocks_ok = true;
    for (int line = 1; line <= 8; line++) {
        ppu_runLine(ppu, line);
        const int row = line <= 4 ? 1 : 5;
        const uint8_t *p = pixels + (size_t)(line - 1) * kPitch;
        const uint8_t want = ppu->brightnessMult[(row + 1) * 3];
        if (p[2] != want) blocks_ok = false;
    }
    failures += check(blocks_ok, "vertical mosaic blocks start at line 1");

    ppu_free(ppu);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ppu_mosaic_table_test: ok\n");
    return 0;
}
