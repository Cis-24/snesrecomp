/* ROM-free test of the generic frame driver's interrupt entry: the real driver
 * over a stubbed CPU bridge, bus, DMA and PPU. Every way it delivers an
 * interrupt (NMI at the frame start, an IRQ latched while the guest runs, the
 * raster IRQ of the draw walk) must enter the handler as the hardware does:
 * P pushed as it was, then I set and D cleared. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "generic_frame_driver.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/snes.h"

CpuState g_cpu;
Snes *g_snes;
Ppu *g_ppu;
uint8 g_snesrecomp_last_hdmaen;

static Snes s_snes;
static Dma s_dma;
static uint8_t s_bus[0x1000000];

/* Bus. */
uint8_t snes_read(Snes *snes, uint32_t adr) { (void)snes; return s_bus[adr & 0xFFFFFFu]; }
void snes_write(Snes *snes, uint32_t adr, uint8_t val) { (void)snes; s_bus[adr & 0xFFFFFFu] = val; }
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
  (void)cpu; s_bus[((uint32_t)bank << 16) | addr] = v;
}

/* DMA and PPU: the walk only has to reach the raster IRQ's line. */
void dma_startDma(Dma *dma, uint8_t val, bool hdma) { (void)dma; (void)val; (void)hdma; }
void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) { (void)c; (void)dc; }
void SimpleHdma_DoLine(SimpleHdma *c) { (void)c; }
void ppu_runLine(Ppu *ppu, int line) { (void)ppu; (void)line; }

/* CPU bridge. The guest runs out each slice where it stands; one slice may
 * latch the raster IRQ, as a V match while the guest runs does. */
static uint64_t s_deadline;
static uint32_t s_pc;
static bool s_latch_irq_in_slice;
static int s_interrupts;
static uint32_t s_entry_pc;
static uint8_t s_entry_flag_i, s_entry_flag_d, s_entry_p, s_entry_pushed_p;

void interp_bridge_set_master_deadline(uint64_t master_clock) { s_deadline = master_clock; }
uint32_t interp_bridge_lle_resume_pc(void) { return s_pc; }
int interp_bridge_lle_took_wai(void) { return 0; }

int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  s_pc = entry_pc24;
  if (s_latch_irq_in_slice) {
    s_latch_irq_in_slice = false;
    s_snes.inIrq = true;
    cpu->master_cycles += 1000;
    return 0;
  }
  if (s_deadline)
    cpu->master_cycles = s_deadline;
  return 0;
}

/* Records what the handler finds, then acknowledges its line. */
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24) {
  s_entry_pc = entry_pc24;
  s_entry_flag_i = cpu->_flag_I;
  s_entry_flag_d = cpu->_flag_D;
  s_entry_p = cpu->P;
  s_entry_pushed_p = s_bus[(uint16_t)(cpu->S + 1u)];
  s_interrupts++;
  if (!s_snes.inNmi)
    s_snes.inIrq = false;
  cpu->master_cycles += 100;
  return 0;
}

static int check(bool condition, const char *message) {
  if (!condition) fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

/* The interrupted code runs with I clear and D set (N, M, X and C set too). */
#define INTERRUPTED_P 0xB9u

static void set_interrupted_flags(void) {
  g_cpu.emulation = 0;
  g_cpu.P = INTERRUPTED_P;
  cpu_p_to_mirrors(&g_cpu);
  s_interrupts = 0;
  s_entry_pc = 0;
}

static int check_entry(const char *what, uint32_t vector) {
  int failures = 0;
  char msg[160];
  snprintf(msg, sizeof msg, "%s: delivered once at its vector", what);
  failures += check(s_interrupts == 1 && s_entry_pc == vector, msg);
  snprintf(msg, sizeof msg, "%s: the handler runs with I set", what);
  failures += check(s_entry_flag_i == 1, msg);
  snprintf(msg, sizeof msg, "%s: the handler runs with D clear", what);
  failures += check(s_entry_flag_d == 0, msg);
  snprintf(msg, sizeof msg, "%s: P agrees with the flag mirrors", what);
  failures += check(s_entry_p == ((INTERRUPTED_P | 0x04u) & ~0x08u), msg);
  snprintf(msg, sizeof msg, "%s: the pushed P is the interrupted code's", what);
  failures += check(s_entry_pushed_p == INTERRUPTED_P, msg);
  if (failures)
    fprintf(stderr, "  %s entry: n=%d pc=$%06X I=%u D=%u P=$%02X pushed P=$%02X\n",
            what, s_interrupts, s_entry_pc, s_entry_flag_i, s_entry_flag_d,
            s_entry_p, s_entry_pushed_p);
  return failures;
}

static void boot(void) {
  snes_generic_frame_driver_reset();
  s_snes.nmiEnabled = false;
  s_snes.vIrqEnabled = false;
  s_snes.inIrq = false;
  snes_generic_frame_driver_run_frame();
}

int main(void) {
  int failures = 0;

  memset(&s_snes, 0, sizeof s_snes);
  s_snes.dma = &s_dma;
  g_snes = &s_snes;
  g_cpu.S = 0x1FF;
  s_bus[0xFFFC] = 0x00; s_bus[0xFFFD] = 0x80; /* reset */
  s_bus[0xFFEA] = 0x00; s_bus[0xFFEB] = 0x15; /* NMI */
  s_bus[0xFFEE] = 0x04; s_bus[0xFFEF] = 0x15; /* IRQ */

  boot();
  set_interrupted_flags();
  s_snes.nmiEnabled = true;
  snes_generic_frame_driver_run_frame();
  failures += check_entry("NMI", 0x1500u);

  boot();
  set_interrupted_flags();
  s_latch_irq_in_slice = true;
  snes_generic_frame_driver_run_frame();
  failures += check_entry("IRQ latched in the frame", 0x1504u);

  boot();
  set_interrupted_flags();
  s_snes.vIrqEnabled = true;
  s_snes.vTimer = 100;
  snes_generic_frame_driver_draw_ppu_frame();
  failures += check_entry("raster IRQ of the draw walk", 0x1504u);

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("generic_frame_driver_test: ok");
  return 0;
}
