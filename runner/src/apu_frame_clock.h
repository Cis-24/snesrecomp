#pragma once
#include <stdint.h>

#define RTL_MASTER_CYCLES_PER_FRAME 357368ull
#define RTL_APU_CYCLES_PER_FRAME 17088ull

/* Reset to the first vblank: the beam starts at V=0 and the first NMI comes
 * at V=225 (overscan is off after reset), 225 lines of 1364 master clocks. */
#define RTL_MASTER_CYCLES_TO_FIRST_VBLANK (225ull * 1364ull)
#define RTL_APU_CYCLES_TO_FIRST_VBLANK                                    \
  (RTL_MASTER_CYCLES_TO_FIRST_VBLANK * RTL_APU_CYCLES_PER_FRAME /         \
   RTL_MASTER_CYCLES_PER_FRAME)

/* A host iteration can include an NMI-disabled loader spanning many hardware
 * frames. Keep its elapsed time when beginning the following iteration. */
typedef struct RtlApuFrameClock {
  uint64_t start_master, start_guest, next_guest, last_duration;
} RtlApuFrameClock;

static inline void rtl_apu_clock_begin(RtlApuFrameClock *clock, uint64_t master) {
  clock->start_master = master;
  clock->start_guest = clock->next_guest;
}

static inline uint64_t rtl_apu_clock_now(const RtlApuFrameClock *clock,
                                        uint64_t master) {
  /* RESET may initialize CPU state inside the first host iteration. */
  uint64_t within = master >= clock->start_master ? master - clock->start_master : 0;
  return clock->start_guest + within * RTL_APU_CYCLES_PER_FRAME /
                               RTL_MASTER_CYCLES_PER_FRAME;
}

static inline uint64_t rtl_apu_clock_finish(RtlApuFrameClock *clock,
                                           uint64_t master) {
  uint64_t end = rtl_apu_clock_now(clock, master);
  /* WAI still advances a hardware frame. The first frame after reset (no
   * frame finished yet, last_duration 0) ends at the first vblank instead:
   * a beam runner stops it at V=225, and a whole field there credited the
   * SPC with 2413 cycles that never elapsed. */
  uint64_t minimum = clock->start_guest +
      (clock->last_duration ? RTL_APU_CYCLES_PER_FRAME
                            : RTL_APU_CYCLES_TO_FIRST_VBLANK);
  if (end < minimum) end = minimum;
  clock->last_duration = end - clock->start_guest;
  clock->next_guest = end;
  /* Leave the current origin intact: raster IRQs can still touch APU ports
   * between the completed game iteration and the next clock_begin. */
  return end;
}
