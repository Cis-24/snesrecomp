/* The APU timeline of the first frames after reset, through the runtime's
 * own RtlRunFrame and frame-boundary sync. common_rtl.c is included whole, as
 * in audio_delivery_test.c; whole-program LTO drops the host entry points the
 * test does not reach, and the stubs below stand in for the frame observers.
 *
 * On hardware the SPC runs from reset. A beam-driven runner (extended frame
 * timing) ends its first frame at the first vblank, V=225: 225 lines of
 * master clocks, not a whole field. Both have to hold before the CPU's first
 * APU port write; FF6's comes about 5888 SPC cycles into the first frame. */
#include <stdio.h>
#include "../../runner/src/common_rtl.c"
#include "../../runner/src/snes/dsp_shadow.h"
/* MinGW's sdl2.pc passes -Dmain=SDL_main; this test has its own entry. */
#undef main

/* One thread; only the production host supplies a real mutex. */
static int apu_lock_depth;
void RtlApuLock(void) { ++apu_lock_depth; }
void RtlApuUnlock(void) { --apu_lock_depth; }
SpcPlayer *g_spc_player;
Snes *g_snes;

/* Runtime state and frame observers RtlRunFrame reaches; none of the
 * observers touches the APU. */
CpuState g_cpu;
const RtlGameInfo *g_rtl_game_info;
jmp_buf g_watchdog_jmp;
FrameDumpCallback g_framedump_callback;
int snes_frame_counter;
uint64_t g_apu_timer0_total_ticks;
void WatchdogFrameStart(void) {}
void ppudma_frame_snapshot(int frame) { (void)frame; }
void Tier2CoverageTick(int frame) { (void)frame; }
int snes_runahead_active(void) { return 0; }
uint64_t snesrecomp_host_now_ns(void) { return 0; }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *out_left, int *out_right) {
  (void)shadow; (void)dsp;
  *out_left = canonical_left;
  *out_right = canonical_right;
}
/* The touch-based catch-up belongs to boot before the frame timeline; with
 * a frame running, the SPC must only follow guest time. */
static int legacy_catchups;
void snes_catchupApu(Snes *snes) { (void)snes; legacy_catchups++; }

/* The frame body: master clocks it runs, and optionally one port write. */
static uint64_t frame_master;
static int64_t write_at = -1;
static void run_frame(void) {
  uint64_t start = g_cpu.master_cycles;
  if (write_at >= 0) {
    g_cpu.master_cycles = start + (uint64_t)write_at;
    RtlApuWrite(0x2140, 0xcc);
    write_at = -1;
  }
  g_cpu.master_cycles = start + frame_master;
}
static const RtlGameInfo game = { .run_frame = run_frame };

static int failures;
static void check(int ok, const char *what, uint64_t got, uint64_t want) {
  if (ok) return;
  fprintf(stderr, "FAIL: %s (got %llu, want %llu)\n", what,
          (unsigned long long)got, (unsigned long long)want);
  failures++;
}
static void expect_clock(const char *what, uint64_t want) {
  check(g_snes->apu->portClock == want, what, g_snes->apu->portClock, want);
}

/* Power-on: fresh APU, zeroed host pacing, beam at V=0. */
static void power_on(void) {
  apu_reset(g_snes->apu);
  snes_frame_counter = 0;
  g_apu_frame_time_valid = false;
  g_apu_frame_clock = (RtlApuFrameClock){0};
  g_apu_frame_start_master = g_cpu.master_cycles;
}

static void run(uint64_t master) {
  frame_master = master;
  RtlRunFrame(0);
  check(apu_lock_depth == 0, "APU lock released", (uint64_t)apu_lock_depth, 0);
}

/* Reset to V=225 as the beam frame driver measures it (306902 master clocks
 * in FF6), and the SPC cycles that span is worth. */
#define FIRST_FRAME_MASTER 306902ull
#define FIRST_FRAME_SPC \
  (FIRST_FRAME_MASTER * RTL_APU_CYCLES_PER_FRAME / RTL_MASTER_CYCLES_PER_FRAME)

int main(void) {
  static Snes machine;
  machine.apu = apu_init();
  g_snes = &machine;
  g_rtl_game_info = &game;
  RtlEnableExtendedFrameTiming();

  /* No port write at all: the SPC still runs from reset. */
  power_on();
  run(FIRST_FRAME_MASTER);
  expect_clock("first frame without a port write runs the SPC to V=225",
               FIRST_FRAME_SPC);
  run(RTL_MASTER_CYCLES_PER_FRAME);
  expect_clock("second frame adds one field", FIRST_FRAME_SPC + RTL_APU_CYCLES_PER_FRAME);

  /* FF6's boot: the first write comes inside the first frame. It must land at
   * its own guest time, and the frame must still end at V=225. */
  power_on();
  write_at = 123139;  /* 5888 SPC cycles after reset */
  run(FIRST_FRAME_MASTER);
  check(machine.apu->inPorts[0] == 0xcc, "first port write landed",
        machine.apu->inPorts[0], 0xcc);
  expect_clock("first frame with a port write ends at V=225", FIRST_FRAME_SPC);
  run(RTL_MASTER_CYCLES_PER_FRAME);
  expect_clock("second frame after the write adds one field",
               FIRST_FRAME_SPC + RTL_APU_CYCLES_PER_FRAME);

  /* The write is timed against the reset anchor: scheduled at the guest cycle
   * of its master position, not at whatever the SPC had run when it came. */
  power_on();
  write_at = 123139;
  run(FIRST_FRAME_MASTER);
  check(machine.apu->portLastTarget == 5888,
        "first port write is scheduled at its guest cycle since reset",
        machine.apu->portLastTarget, 5888);

  /* WAI still advances a hardware frame after the first one. */
  power_on();
  run(FIRST_FRAME_MASTER);
  run(1000);
  expect_clock("a WAI-short later frame still counts a field",
               FIRST_FRAME_SPC + RTL_APU_CYCLES_PER_FRAME);

  /* A first frame cut short by WAI still covers reset to the first vblank. */
  power_on();
  run(1000);
  expect_clock("a WAI-short first frame reaches the first vblank",
               225ull * 1364ull * RTL_APU_CYCLES_PER_FRAME / RTL_MASTER_CYCLES_PER_FRAME);

  check(legacy_catchups == 0, "no touch-based catch-up inside frames",
        (uint64_t)legacy_catchups, 0);
  apu_free(machine.apu);
  if (failures) {
    fprintf(stderr, "apu_frame_timeline_test: %d FAIL\n", failures);
    return 1;
  }
  puts("apu_frame_timeline_test: PASS");
  return 0;
}
