/* Regression: Mode 2 offset-per-tile reads its rows at BG3VOFS and BG3VOFS + 8,
 * and a V-offset entry replaces BG1VOFS so that line y shows row entry + y.
 *
 * The renderer took snes9x's `VOffset - 1` and `VCellOffset + 1` over to raw
 * BGnVOFS values and 1-based lines. snes9x stores BGnVOFS + 1 per line and
 * counts lines from 0, so both corrections were one line off here: with
 * BG3VOFS = 0 the V offsets came from the H row (FF6's slot reels, battle
 * Slot command, stood still at offset 0), and an offset column sat one line
 * low.
 *
 * BG1 tile rows identify the sampled source: red is the source row mod 32,
 * green the parity of the source tile column. No game ROM, generated data,
 * or platform frontend is required. */
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

enum {
    kPitch = kPpuXPixels * 4,
    kBg3Map = 0x0400,   /* BG3SC = $04 */
    kBg1Chr = 0x1000,   /* BG12NBA = $01 */
    kHOffset = 8,       /* H entry: BG1 moves one tile to the right */
    kVOffset = 16,      /* V entry: replaces BG1VOFS */
    kBg1Vofs = 3,
};

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

static void write_scroll(Ppu *ppu, int reg, unsigned value) {
    ppu_write(ppu, (uint8_t)reg, (uint8_t)(value & 0xff));
    ppu_write(ppu, (uint8_t)reg, (uint8_t)(value >> 8));
}

/* BG1 alone on the main screen, Mode 2. Tile 1 has colour k + 1 on pixel row
 * k; the map entry's palette carries source tile row & 3 and column parity,
 * and CGRAM turns that back into red = source row & 31, green = 1 + parity. */
static void setup(Ppu *ppu, int h_row, int bg3vofs) {
    ppu_reset(ppu);
    ppu->inidisp = 0x0f;
    ppu->bgmode = 2;
    ppu->screenEnabled[0] = 0x01;
    ppu->bgXsc[0] = 0x00;
    ppu->bgXsc[2] = kBg3Map >> 8;
    ppu->bgTileAdr = kBg1Chr >> 12;
    memset(ppu->vram, 0, sizeof ppu->vram);
    memset(ppu->cgram, 0, sizeof ppu->cgram);

    for (int k = 0; k < 8; k++) {
        const int color = k + 1;
        const uint16_t lo =
            (uint16_t)((color & 1 ? 0x00ff : 0) | (color & 2 ? 0xff00 : 0));
        const uint16_t hi =
            (uint16_t)((color & 4 ? 0x00ff : 0) | (color & 8 ? 0xff00 : 0));
        ppu->vram[kBg1Chr + 16 + k] = lo;
        ppu->vram[kBg1Chr + 16 + 8 + k] = hi;
    }
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) {
            const int pal = (r & 3) | ((c & 1) << 2);
            ppu->vram[r * 32 + c] = (uint16_t)(1 | (pal << 10));
        }
    }
    for (int pal = 0; pal < 8; pal++) {
        for (int k = 0; k < 8; k++) {
            const int red = (pal & 3) * 8 + k;
            const int green = 1 + (pal >> 2);
            ppu->cgram[pal * 16 + k + 1] = (uint16_t)(red | (green << 5));
        }
    }

    /* OPT rows: H entries in h_row, V entries in the row below it. The row
     * above h_row (or the last one) carries junk with the enable bit set,
     * read as H it gives the odd tile column and read as V another row, so
     * a wrong row shows up as a wrong pixel. */
    for (int c = 0; c < 32; c++) {
        ppu->vram[kBg3Map + h_row * 32 + c] = 0x2000 | kHOffset;
        ppu->vram[kBg3Map + (h_row + 1) * 32 + c] = 0x2000 | kVOffset;
        ppu->vram[kBg3Map + ((h_row + 31) & 31) * 32 + c] = 0x2000 | 0x20;
    }
    write_scroll(ppu, 0x0e, kBg1Vofs);  /* BG1VOFS */
    write_scroll(ppu, 0x12, (unsigned)bg3vofs);  /* BG3VOFS */
}

/* The pixel at (x, line) came from BG1 source row `row` and a tile column of
 * parity `parity`. */
static bool pixel_is(const Ppu *ppu, const uint8_t *pixels, int line, int x,
                     int row, int parity) {
    const uint8_t *p = pixels + (size_t)(line - 1) * kPitch + (size_t)x * 4;
    return p[2] == ppu->brightnessMult[row & 31] &&
           p[1] == ppu->brightnessMult[1 + parity];
}

static int run_case(Ppu *ppu, int h_row, int bg3vofs, const char *name) {
    static uint8_t pixels[kPitch * 8];
    int failures = 0;
    setup(ppu, h_row, bg3vofs);
    memset(pixels, 0, sizeof pixels);
    PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
    bool left_ok = true, h_ok = true, v_ok = true;
    for (int line = 1; line <= 8; line++) {
        ppu_runLine(ppu, line);
        /* Column 0 is never offset: BG1VOFS + line, tile column 0. */
        for (int x = 0; x < 8; x++)
            if (!pixel_is(ppu, pixels, line, x, kBg1Vofs + line, 0)) left_ok = false;
        for (int x = 8; x < 16; x++) {
            /* H entry: source x = 8 + x, tile column 2 (parity 0); without
             * it the column would be 1. V entry: row kVOffset + line. */
            const uint8_t *p = pixels + (size_t)(line - 1) * kPitch + (size_t)x * 4;
            if (p[1] != ppu->brightnessMult[1]) h_ok = false;
            if (p[2] != ppu->brightnessMult[(kVOffset + line) & 31]) v_ok = false;
        }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%s: the leftmost column keeps BG1's own scroll", name);
    failures += check(left_ok, msg);
    snprintf(msg, sizeof msg, "%s: H offsets come from the row at BG3VOFS", name);
    failures += check(h_ok, msg);
    snprintf(msg, sizeof msg, "%s: V offsets come from the row at BG3VOFS + 8 and give row entry + line", name);
    failures += check(v_ok, msg);
    return failures;
}

int main(void) {
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;

    failures += run_case(ppu, 0, 0, "BG3VOFS = 0");
    failures += run_case(ppu, 1, 8, "BG3VOFS = 8");
    failures += run_case(ppu, 1, 12, "BG3VOFS = 12");

    if (failures == 0) printf("ppu_mode2_opt_test: OK\n");
    return failures ? 1 : 0;
}
