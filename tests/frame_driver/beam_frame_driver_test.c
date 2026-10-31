/* ROM-free test of the beam frame driver: the real driver, DMA unit, raster
 * journal and PPU register file over a stubbed CPU, beam and bus. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beam_frame_driver.h"
#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/cx4.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include "snes/snes.h"

CpuState g_cpu;
Snes *g_snes;
Ppu *g_ppu;
bool g_fail;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

static Snes s_snes;
static uint8_t s_bus[0x1000000];

/* The widescreen shadow layer belongs to the host; ppu.c only asks it. */
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t tile) {
  (void)layer; (void)x; (void)y; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) { (void)layer; (void)x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) { (void)address; (void)value; }

/* Bus. Program bytes and HDMA tables live in s_bus; the B bus is the PPU. */
uint8_t snes_read(Snes *snes, uint32_t adr) { (void)snes; return s_bus[adr & 0xFFFFFFu]; }
void snes_write(Snes *snes, uint32_t adr, uint8_t val) { (void)snes; s_bus[adr & 0xFFFFFFu] = val; }
uint8_t snes_readBBus(Snes *snes, uint8_t adr) { (void)snes; (void)adr; return 0; }
void snes_writeBBus(Snes *snes, uint8_t adr, uint8_t val) { (void)snes; ppu_write(g_ppu, adr, val); }
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
  (void)cpu; s_bus[((uint32_t)bank << 16) | addr] = v;
}

/* Beam. The driver only needs it to reach the boundary it asks for. */
void snes_refresh_exempt(void) {}
void snes_set_hdma_beam_enabled(Snes *snes, bool enabled) { (void)snes; (void)enabled; }
void snes_set_raster_irq_beam_enabled(bool enabled) { (void)enabled; }
uint32_t snes_master_clocks_until_irq(const Snes *snes) { (void)snes; return 0; }
uint32_t snes_master_clocks_until_line(const Snes *snes, uint32_t line) {
  (void)snes; (void)line; return 0;
}
void snes_sync_master_clock(Snes *snes, uint64_t master_clock) { snes->beamMasterLast = master_clock; }
void snes_advance_master_cycles(Snes *snes, uint32_t master_cycles) { snes->beamMasterLast += master_cycles; }

/* No coprocessor on this cartridge. */
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) { (void)cart; (void)master_clock; }
int cx4_irq_pending(const Cx4 *cx4) { (void)cx4; return 0; }
bool sa1_cpu_irq_pending(const Sa1 *sa1) { (void)sa1; return false; }

/* CPU: runs out each slice parked where it stands, except for the one-clock
 * slice that means "one instruction", which executes the poll for real. */
static uint64_t s_deadline;
static uint32_t s_pc;
static uint32_t s_park_at;
static bool s_negative;
static int s_steps;
static bool s_poll_saw_flag;
static int s_nmis;
static uint32_t s_nmi_return;

void interp_bridge_set_master_deadline(uint64_t master_clock) { s_deadline = master_clock; }
uint32_t interp_bridge_lle_resume_pc(void) { return s_pc; }
int interp_bridge_lle_took_wai(void) { return 0; }

static void step_poll_instruction(void) {
  const uint8_t op = s_bus[s_pc];
  if (op == 0xADu || op == 0x2Cu) { /* lda/bit $4210 */
    s_negative = s_snes.rdnmiPending;
    s_snes.rdnmiPending = false;
    s_poll_saw_flag |= s_negative;
    s_pc += 3;
  } else if (op == 0x10u) { /* bpl */
    const int8_t rel = (int8_t)s_bus[s_pc + 1];
    s_pc += s_negative ? 2u : (uint32_t)(int32_t)(2 + rel);
  } else {
    fprintf(stderr, "single-stepped something other than the poll at %06X\n", s_pc);
    exit(1);
  }
}

int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  s_pc = entry_pc24;
  if (s_deadline == cpu->master_cycles + 1u) {
    step_poll_instruction();
    cpu->master_cycles += 6;
    s_steps++;
    return 0;
  }
  if (s_park_at) {
    s_pc = s_park_at;
    s_park_at = 0;
  }
  cpu->master_cycles = s_deadline;
  return 0;
}

/* The handler acknowledges RDNMI first thing, as FF6's FieldNMI does. */
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24) {
  (void)entry_pc24;
  s_nmis++;
  s_nmi_return = s_pc;
  s_snes.rdnmiPending = false;
  cpu->master_cycles += 100;
  return 0;
}

static int check(bool condition, const char *message) {
  if (!condition) fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

/* Parks the CPU on `park` in the boot frame, then runs the frame whose NMI finds
 * it there with RDNMI as given. */
static void run_nmi_frame(uint32_t park, bool rdnmi, bool nmi_enabled) {
  snes_beam_frame_driver_reset();
  s_snes.nmiEnabled = false;
  s_snes.rdnmiPending = false;
  s_park_at = park;
  snes_beam_frame_driver_run_frame();

  s_snes.nmiEnabled = nmi_enabled;
  s_snes.rdnmiPending = rdnmi;
  s_negative = false;
  s_steps = 0;
  s_poll_saw_flag = false;
  s_nmis = 0;
  s_nmi_return = 0;
  snes_beam_frame_driver_run_frame();
}

/* EventCmd_f0's wait after a sound command ($C0B7A0): lda $4210 / bpl -5. */
#define POLL_LDA 0xC0B7A0u
#define POLL_BPL 0xC0B7A3u
/* The same poll with bit, and a loop that closes on something else. */
#define POLL_BIT 0xC10000u
#define OTHER_LDA 0xC20000u
#define OTHER_BPL 0xC20003u
#define PLAIN_CODE 0xC30000u

static void write_program(void) {
  static const uint8_t lda_poll[] = {0xAD, 0x10, 0x42, 0x10, 0xFB, 0xEA};
  static const uint8_t bit_poll[] = {0x2C, 0x10, 0x42, 0x10, 0xFB, 0xEA};
  static const uint8_t other[] = {0xAD, 0x12, 0x42, 0x10, 0xFB, 0xEA};
  memcpy(&s_bus[POLL_LDA], lda_poll, sizeof lda_poll);
  memcpy(&s_bus[POLL_BIT], bit_poll, sizeof bit_poll);
  memcpy(&s_bus[OTHER_LDA], other, sizeof other);
  s_bus[PLAIN_CODE] = 0xEA;
  s_bus[0xFFFC] = 0x00; s_bus[0xFFFD] = 0x80; /* reset */
  s_bus[0xFFEA] = 0x00; s_bus[0xFFEB] = 0x15; /* NMI */
}

/* RDNMI rises a moment before the CPU takes the NMI. Parked at that boundary, a
 * poll has to run in the window or its handler's acknowledge starves it. */
static int test_rdnmi_poll_runs_before_nmi(void) {
  int failures = 0;

  run_nmi_frame(POLL_LDA, true, true);
  failures += check(s_nmis == 1, "parked on lda: NMI still delivered");
  failures += check(s_poll_saw_flag, "parked on lda: the poll reads the flag before the NMI");
  failures += check(s_steps == 1, "parked on lda: one instruction runs");
  failures += check(s_nmi_return == POLL_BPL, "parked on lda: the NMI returns to the bpl");

  run_nmi_frame(POLL_BPL, true, true);
  failures += check(s_nmis == 1, "parked on bpl: NMI still delivered");
  failures += check(s_poll_saw_flag, "parked on bpl: the poll reads the flag before the NMI");
  failures += check(s_steps == 2, "parked on bpl: bpl and lda run");
  failures += check(s_nmi_return == POLL_BPL, "parked on bpl: the NMI returns to the bpl");

  run_nmi_frame(POLL_BIT, true, true);
  failures += check(s_poll_saw_flag, "bit $4210 polls the same way");

  /* Everything else is delivered where it is parked. */
  run_nmi_frame(PLAIN_CODE, true, true);
  failures += check(s_steps == 0 && s_nmi_return == PLAIN_CODE, "other code is not stepped");

  run_nmi_frame(OTHER_LDA, true, true);
  failures += check(s_steps == 0 && s_nmi_return == OTHER_LDA, "a read of another register is not a poll");

  run_nmi_frame(OTHER_BPL, true, true);
  failures += check(s_steps == 0 && s_nmi_return == OTHER_BPL, "a bpl closing another loop is not a poll");

  run_nmi_frame(POLL_LDA, false, true);
  failures += check(s_steps == 0 && s_nmi_return == POLL_LDA, "no flag up: nothing to catch");

  run_nmi_frame(POLL_LDA, true, false);
  failures += check(s_steps == 0 && s_nmis == 0, "NMI off: the poll just keeps running");

  return failures;
}

/* HDMA over one rendered field. Channel 0 drives WH0 ($2126) and the observer
 * records what each line was actually drawn with. */
#define HDMA_TABLE 0x7E0100u
#define WH0_BASELINE 0xFFu
static uint8_t s_line_wh0[225];
static uint8_t s_pixels[kPpuXPixels * 4 * 225];

static void observe_line(const struct Ppu *ppu, unsigned line, void *context) {
  (void)context;
  s_line_wh0[line] = ppu->window1left;
}

/* Field journal empty, register file at the baseline, channel programmed. */
static void begin_field(const uint8_t *table, size_t size) {
  Dma *dma = s_snes.dma;
  dma_reset(dma);
  memcpy(&s_bus[HDMA_TABLE], table, size);
  dma_write(dma, 0x4300, 0x00);
  dma_write(dma, 0x4301, 0x26);
  dma_write(dma, 0x4302, (uint8_t)HDMA_TABLE);
  dma_write(dma, 0x4303, (uint8_t)(HDMA_TABLE >> 8));
  dma_write(dma, 0x4304, (uint8_t)(HDMA_TABLE >> 16));
  ppu_rasterReset();
  ppu_write(g_ppu, 0x26, WH0_BASELINE);
  ppu_rasterBegin(g_ppu);
}

static void render_field(void) {
  memset(s_line_wh0, 0, sizeof s_line_wh0);
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  PpuBeginDrawing(g_ppu, s_pixels, kPpuXPixels * 4, 0);
  snes_beam_frame_driver_draw_ppu_frame_observed(observe_line, NULL);
}

/* Two lines of $11 without repeat, then one of $22. */
static const uint8_t kStepTable[] = {0x02, 0x11, 0x01, 0x22, 0x00};
/* Two lines in repeat mode: $33, then $44. */
static const uint8_t kRepeatTable[] = {0x82, 0x33, 0x44, 0x00};

/* Line 0's H-blank is a full step. The table's first entry shows on lines 1..N;
 * without the decrement it kept them for N+1 and every later step came late. */
static int test_hdma_steps_from_line_zero(void) {
  int failures = 0;

  begin_field(kStepTable, sizeof kStepTable);
  dma_startDma(s_snes.dma, 0x01, true);
  render_field();
  failures += check(s_line_wh0[1] == 0x11, "step table: line 1 has the first entry");
  failures += check(s_line_wh0[2] == 0x11, "step table: line 2 still has it");
  failures += check(s_line_wh0[3] == 0x22, "step table: line 3 has the second entry");
  failures += check(s_line_wh0[224] == 0x22, "step table: the last value holds");

  begin_field(kRepeatTable, sizeof kRepeatTable);
  dma_startDma(s_snes.dma, 0x01, true);
  render_field();
  failures += check(s_line_wh0[1] == 0x33, "repeat table: line 1 has the first byte");
  failures += check(s_line_wh0[2] == 0x44, "repeat table: line 2 has the second byte");
  return failures;
}

/* FF6 turns HDMA off in a raster IRQ near the bottom of each field and on again
 * in NMI. The vblank write comes before V=0 and arms the channel for the init;
 * read from line 1 on, it started every table two lines late. */
static int test_hdma_armed_by_a_vblank_write(void) {
  int failures = 0;

  begin_field(kStepTable, sizeof kStepTable);
  for (int field = 0; field < 2; field++) {
    ppu_rasterRecord(0x420C, 230, 0x01);
    ppu_rasterRecord(0x420C, 220, 0x00);
    render_field();
    failures += check(s_line_wh0[1] == 0x11, "vblank enable: line 1 has the first entry");
    failures += check(s_line_wh0[2] == 0x11, "vblank enable: line 2 still has it");
    failures += check(s_line_wh0[3] == 0x22, "vblank enable: line 3 has the second entry");
    failures += check(!s_snes.dma->channel[0].hdmaActive, "the raster IRQ's disable still lands");
  }
  return failures;
}

/* A channel switched on part-way down the field still spends its first slot on
 * the table header, so applying the vblank writes early must not move this. */
static int test_hdma_mid_field_enable_keeps_its_delay(void) {
  int failures = 0;

  begin_field(kStepTable, sizeof kStepTable);
  ppu_rasterRecord(0x420C, 21, 0x01);
  render_field();
  failures += check(s_line_wh0[1] == WH0_BASELINE, "mid-field enable: top of the field untouched");
  failures += check(s_line_wh0[23] == WH0_BASELINE, "mid-field enable: first slot only loads the header");
  failures += check(s_line_wh0[24] == 0x11, "mid-field enable: first transfer in the slot after");
  return failures;
}

/* A guest write to a channel register, as snes.c does it. */
static void guest_channel_write(uint16_t line, uint16_t reg, uint8_t val) {
  ppu_rasterRecord(reg, line, val);
  dma_write(s_snes.dma, reg, val);
}

/* FF6's frame: NMI sets channel 0 up for HDMA and arms it, the raster IRQ at
 * V=215 switches HDMA off and reuses the channel for a VRAM upload. The walk
 * runs after all of it and has to see the channel as it was on each line. */
static int test_hdma_sees_the_channel_of_its_line(void) {
  Dma *dma = s_snes.dma;
  int failures = 0;

  begin_field(kStepTable, sizeof kStepTable);
  for (int field = 0; field < 2; field++) {
    guest_channel_write(230, 0x4300, 0x00);
    guest_channel_write(230, 0x4301, 0x26);
    ppu_rasterRecord(0x420C, 231, 0x01);
    ppu_rasterRecord(0x420C, 215, 0x00);
    guest_channel_write(215, 0x4300, 0x41);
    guest_channel_write(215, 0x4301, 0x18);
    const uint32_t vram_writes = g_ppu->vramWriteCount;
    render_field();
    failures += check(s_line_wh0[1] == 0x11, "reused channel: line 1 has the first entry");
    failures += check(s_line_wh0[3] == 0x22, "reused channel: line 3 has the second entry");
    failures += check(g_ppu->vramWriteCount == vram_writes,
                      "reused channel: the walk does not write the VRAM port");
    failures += check(dma->channel[0].bAdr == 0x18 && dma->channel[0].mode == 1,
                      "the guest gets its channel registers back");
  }

  /* A split: the channel changes register on a visible line, with HDMA running.
   * Table: 6 lines each on its own transfer. */
  static const uint8_t kSplitTable[] = {0x86, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00};
  begin_field(kSplitTable, sizeof kSplitTable);
  ppu_write(g_ppu, 0x27, WH0_BASELINE);
  ppu_rasterRecord(0x420C, 230, 0x01);
  guest_channel_write(2, 0x4301, 0x27);
  guest_channel_write(150, 0x4301, 0x26);
  render_field();
  failures += check(s_line_wh0[3] == 0x33, "split: before it the channel still drives $2126");
  failures += check(s_line_wh0[4] == 0x33, "split: after it $2126 keeps its value");
  failures += check(g_ppu->window1right == 0x36, "split: the rest of the table went to $2127");
  failures += check(dma->channel[0].bAdr == 0x26, "split: the last write is what the guest has");
  return failures;
}

int main(void) {
  int failures = 0;

  memset(&s_snes, 0, sizeof s_snes);
  g_snes = &s_snes;
  s_snes.cpu = NULL;
  g_ppu = ppu_init();
  if (!g_ppu) return 2;
  ppu_reset(g_ppu);
  s_snes.ppu = g_ppu;
  s_snes.dma = dma_init(&s_snes);
  if (!s_snes.dma) return 2;
  dma_reset(s_snes.dma);
  g_cpu.S = 0x1FF;
  write_program();

  failures += test_rdnmi_poll_runs_before_nmi();
  failures += test_hdma_steps_from_line_zero();
  failures += test_hdma_armed_by_a_vblank_write();
  failures += test_hdma_mid_field_enable_keeps_its_delay();
  failures += test_hdma_sees_the_channel_of_its_line();

  dma_free(s_snes.dma);
  ppu_free(g_ppu);
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("beam_frame_driver_test: ok");
  return 0;
}
