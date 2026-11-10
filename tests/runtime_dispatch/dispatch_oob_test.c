/* A dispatch value that is missing from a generated switch must be reported in
 * every build. Generated code calls cpu_trace_dispatch_oob() in the switch's
 * default arm; for a call-form dispatch without a live pointer the arm then
 * unpops the call frame and falls through, so the handler never runs. With
 * SNESRECOMP_TRACE=0 the call used to be an empty inline: FF6's wound handler
 * ($C24600, missing from `jsr (SetStatusTbl,x)` at $C243AC) was skipped for
 * months without a miss, a tripwire or a log line. The production build now
 * records each site in an always-on table (post-mortem JSON) and prints one
 * stderr line on its first hit, like the unresolved-abandon accounting. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "program_module.h"
#include "sha256.h"
#include "snes/tier2_capture.h"

#if SNESRECOMP_TRACE
#error "this test checks the production build (SNESRECOMP_TRACE=0)"
#endif

typedef struct Ppu Ppu;
typedef struct Dma Dma;

CpuState g_cpu;
int snes_frame_counter;
int g_wlog_configured;
uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;

/* common_cpu_infra.c links against the runtime; nothing below may run. */
int wlog_scope_available(void) { return 0; }
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
uint8 *RomPtr(uint32_t addr) { (void)addr; return NULL; }
void Tier2CoverageWriteDefaultManifest(const char *rom_title) { (void)rom_title; }
void tier2_capture_set_default_enabled(int enabled) { (void)enabled; }
int tier2_capture_enabled(void) { return 0; }
void msu1_init(void) {}
Snes *snes_init(uint8_t *ram) { (void)ram; return NULL; }
bool snes_loadRom(Snes *snes, const uint8_t *data, int length) {
  (void)snes; (void)data; (void)length; return false;
}
void snes_reset(Snes *snes, bool hard) { (void)snes; (void)hard; }
void cart_set_master_clock_source(Cart *cart, const uint64_t *master_clock) {
  (void)cart; (void)master_clock;
}
void audio_trace_set_beam_probe(void (*probe)(uint16_t *, uint16_t *)) { (void)probe; }
void ppu_reset(Ppu *ppu) { (void)ppu; }
void dma_reset(Dma *dma) { (void)dma; }
void cpu_state_init(CpuState *cpu, uint8_t *ram) { (void)cpu; (void)ram; }
void rtl_reset_host_pacing(void) {}
void snes_sync_master_clock(Snes *snes, uint64_t clock) { (void)snes; (void)clock; abort(); }
void snes_set_master_clock_charge_hook(SnesMasterClockChargeHook hook) { (void)hook; abort(); }
void snes_set_wram_write_log_hook(SnesWramWriteLogHook hook) { (void)hook; abort(); }
void wlog_addr_note_direct(uint32_t wa, uint8_t v, const char *via) {
  (void)wa; (void)v; (void)via; abort();
}
const char *cpu_dispatch_entry_reason(uint32_t pc, uint8_t mx) { (void)pc; (void)mx; abort(); }
void tier2_capture_set_entry_probe(const char *(*probe)(uint32_t, uint8_t)) { (void)probe; abort(); }
void tier2_capture_set_checkpoint_hook(void (*hook)(void)) { (void)hook; abort(); }
void Tier2CoverageReset(void) { abort(); }
void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
  (void)data; (void)len; (void)out; abort();
}
const SnesProgramModule *snes_program_module_active(void) { abort(); }
const SnesProgramModule *snes_program_module_find(const char *id) { (void)id; abort(); }
void tier2_capture_set_identity(const char *rom, const char *module,
                               const char *program, const char *mapper) {
  (void)rom; (void)module; (void)program; (void)mapper; abort();
}
void tier2_capture_set_build_digest(const char *digest) { (void)digest; abort(); }

/* stderr goes to a file; results go to stdout. */
static int check(int condition, const char *message) {
  if (!condition) printf("FAIL: %s\n", message);
  return condition ? 0 : 1;
}

static size_t slurp(FILE *f, char *buf, size_t cap) {
  size_t n;
  rewind(f);
  n = fread(buf, 1, cap - 1, f);
  buf[n] = 0;
  return n;
}

static int count_lines_with(const char *text, const char *needle) {
  int n = 0;
  for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle))
    n++;
  return n;
}

/* The line that names a site, or NULL. */
static const char *line_of(const char *text, const char *needle) {
  const char *p = strstr(text, needle);
  if (!p) return NULL;
  while (p > text && p[-1] != '\n') p--;
  return p;
}

int main(int argc, char **argv) {
  const char *log_path = argc > 1 ? argv[1] : "dispatch_oob_test.stderr.txt";
  static char log[8192], json[8192];
  int failures = 0;
  FILE *f;

  if (!freopen(log_path, "w+", stderr)) {
    printf("cannot redirect stderr to %s\n", log_path);
    return 2;
  }

  memset(&g_cpu, 0, sizeof g_cpu);
  snes_frame_counter = 41;
  failures += check(cpu_trace_dispatch_oob(&g_cpu, 0xC243AC, 0x4600) == RECOMP_RETURN_NORMAL,
                    "the default arm continues normally");
  snes_frame_counter = 42;
  (void)cpu_trace_dispatch_oob(&g_cpu, 0xC243AC, 0x4600);
  snes_frame_counter = 43;
  (void)cpu_trace_dispatch_oob(&g_cpu, 0xC243AC, 0x4602);
  (void)cpu_trace_dispatch_oob(&g_cpu, 0xC0FFEE, 0x1234);
  fflush(stderr);

  slurp(stderr, log, sizeof log);
  failures += check(count_lines_with(log, "[dispatch-oob]") == 2,
                    "one stderr line per site, on its first hit");
  {
    const char *a = line_of(log, "site=$C243AC");
    const char *b = line_of(log, "site=$C0FFEE");
    failures += check(a && strncmp(a, "[dispatch-oob]", 14) == 0,
                      "the line names the first site");
    failures += check(a && strstr(a, "value=$004600") && strstr(a, "frame=41"),
                      "the line has the value and frame of the first hit");
    failures += check(b && strstr(b, "value=$001234") && strstr(b, "frame=43"),
                      "the second site gets its own line");
  }

  f = tmpfile();
  if (!f) {
    printf("tmpfile failed\n");
    return 2;
  }
  CpuDispatchOobDumpJson(f);
  slurp(f, json, sizeof json);
  fclose(f);
  failures += check(strstr(json, "\"dispatch_oob\": {") != NULL,
                    "the post-mortem JSON has a dispatch_oob section");
  failures += check(strstr(json, "\"total_hits\": 4,") != NULL, "4 hits in total");
  failures += check(strstr(json, "\"distinct_sites\": 2,") != NULL, "2 distinct sites");
  failures += check(strstr(json, "{\"site\": \"0xC243AC\", \"hits\": 3, \"first_frame\": 41, "
                                 "\"last_frame\": 43, \"first_value\": \"0x004600\", "
                                 "\"last_value\": \"0x004602\"}") != NULL,
                    "the first site's record");
  failures += check(strstr(json, "{\"site\": \"0xC0FFEE\", \"hits\": 1, \"first_frame\": 43, "
                                 "\"last_frame\": 43, \"first_value\": \"0x001234\", "
                                 "\"last_value\": \"0x001234\"}") != NULL,
                    "the second site's record");

  if (failures) {
    printf("stderr was:\n%s\njson was:\n%s\n%d check(s) failed\n", log, json, failures);
    return 1;
  }
  puts("dispatch_oob_test: PASS");
  return 0;
}
