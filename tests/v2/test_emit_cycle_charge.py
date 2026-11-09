"""Axis-2 step C: the v2 emitter charges each block's static 65816 CPU
cycles as a per-block constant (recompiler/snes_cycles.py via
emit_function._block_cycle_const). Guards the cost-model -> emitter wiring.

Master clocks are bus-accurate (snes_cycles.py "Bus split"): the block
constant holds the opcode/operand fetches at the code-region speed and the
internal cycles at 6; every data transfer (operand, pointer, stack) goes
through a paced accessor that adds the speed of its own address, and every
runtime-only extra cycle is internal (6). This is how the interpreter prices
time, so a block of generated code costs what the same instructions cost
interpreted."""
import re

from _helpers import make_lorom_bank0  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


# A standalone per-block static charge: `    cpu->cycles += N;` on its own
# line (the dynamic charges are `if (...) cpu->cycles += 1; /* ... */`).
_STATIC_CHARGE = re.compile(r'^\s*cpu->cycles \+= (\d+);\s*$', re.M)
_SPEED = "((g_memsel && (cpu->PB & 0x80)) ? 6 : 8)"
# The block's master constant: fetches x code speed (+ internal x 6).
_MASTER_CHARGE = re.compile(
    r'^\s*cpu->master_cycles \+= (\d+) \* \(\(g_memsel && \(cpu->PB & 0x80\)\) \? 6 : 8\)'
    r'(?: \+ (\d+))?;', re.M)


def _emit(code, m=1, x=1, **kw):
    rom = make_lorom_bank0({0x8000: bytes(code)})
    return emit_function(rom, bank=0, start=0x8000, entry_m=m, entry_x=x, **kw)


def _body(src):
    """The function body after its prologue (the prologue peeks the caller's
    return frame with unpaced reads; that bookkeeping is not guest time)."""
    return src[src.index('cpu_trace_block('):]


def test_linear_block_charges_static_cycles():
    # LDA #$05 (2) ; STA $00 (dp, 3) ; RTS (6) -> one block, 11 static cycles
    # (plus a runtime D.l!=0 dynamic charge for the dp store).
    src = _emit([0xA9, 0x05, 0x85, 0x00, 0x60])
    charges = _STATIC_CHARGE.findall(src)
    assert charges == ['11'], f'expected one static block charge of 11, got {charges}'
    assert "if (cpu->D & 0xFF) { cpu->cycles += 1;" in src
    # Master: 5 fetches (2 + 2 + 1) at the live code speed, 3 internal (RTS)
    # at 6. The STA operand and the two RTS pulls are data, paced as they run.
    charge = f"cpu->master_cycles += 5 * {_SPEED} + 18;"
    assert charge in src, src
    assert src.index(
        "cpu->coprocessor_master_cycles = cpu->master_cycles;"
    ) < src.index(charge), src
    assert "cpu_write8_paced(cpu, 0x00, (uint16)(cpu->D + 0x0000), _v2);" in src
    assert src.count("cpu_read8_paced(cpu, 0x00, cpu->S)") == 2   # RTS pulls


def test_width_widens_static_charge():
    # 16-bit (REP #$30) LDA #$1234 (3) ; RTS (6) -> 9. The native REP itself
    # (CLC/XCE/REP) live in the entry block; assert the 16-bit LDA path adds
    # the m=0 cycle (base 2 + 1). We check the total contains a charge whose
    # value reflects 16-bit accounting (>= the 8-bit equivalent).
    s8 = _emit([0xA9, 0x05, 0x60])                      # LDA# 8b ; RTS
    s16 = _emit([0xC2, 0x20, 0xA9, 0x34, 0x12, 0x60])   # REP#$20; LDA#16b; RTS
    c8 = sum(int(x) for x in re.findall(r'cpu->cycles \+= (\d+);', s8))
    c16 = sum(int(x) for x in re.findall(r'cpu->cycles \+= (\d+);', s16))
    # 8b: LDA# 2 + RTS 6 = 8. 16b: REP 3 + LDA#(2+1) + RTS 6 = 12.
    assert c8 == 8, f'8-bit total {c8} != 8'
    assert c16 == 12, f'16-bit total {c16} != 12 (m=0 LDA should add 1)'
    # The m=0 cycle of an immediate is its third operand byte: a fetch.
    f8 = sum(int(m[0]) for m in _MASTER_CHARGE.findall(s8))
    f16 = sum(int(m[0]) for m in _MASTER_CHARGE.findall(s16))
    assert (f8, f16) == (3, 6), (f8, f16)


def test_dp_dynamic_charge_emitted():
    # LDA $00 (DP mode) ; RTS: the D.l!=0 charge is runtime-conditional and
    # an internal cycle: 6 master clocks whatever the code region.
    src = _emit([0xA5, 0x00, 0x60])
    assert "if (cpu->D & 0xFF) { cpu->cycles += 1; cpu->master_cycles += 6; }" in src, src


def test_abs_x_page_cross_dynamic_charge_emitted():
    # LDA $1234,X (read) ; RTS — page-cross charge uses the static base $1234.
    src = _emit([0xBD, 0x34, 0x12, 0x60])
    assert "0x1234 & 0xFF00" in src and "+ cpu->X) & 0xFF00)" in src, src
    assert ("{ cpu->cycles += 1; cpu->master_cycles += 6; }"
            "  /* abs,X read page-cross */") in src, src


def test_taken_branch_charges_one_cycle():
    # BNE fork: the taken edge must add +1 cycle (block const = not-taken base),
    # an internal cycle at 6 master clocks.
    src = _emit([
        0xD0, 0x02,  # BNE $8004
        0xEA, 0x60,  # NOP; RTS
        0xEA, 0x60,  # NOP; RTS (taken)
    ])
    assert re.search(
        r'if \(.*\) \{ cpu->cycles \+= 1; cpu->master_cycles \+= 6; goto ', src), src


def test_wide_index_read_folds_penalty_without_dynamic_double_count():
    src = _emit([0xBD, 0x34, 0x12, 0x60], x=0)
    assert _STATIC_CHARGE.findall(src) == ['11']  # LDA 5 + RTS 6
    assert 'page-cross' not in src
    # LDA abs,X at x=0: 3 fetches, 1 internal (the indexing cycle), 1 data.
    assert f"cpu->master_cycles += 4 * {_SPEED} + 24;" in src, src


def test_store_abs_x_has_no_page_cross_charge():
    # STA $1234,X (store) — stores pay a fixed cost (in the base), no cross add.
    src = _emit([0x9D, 0x34, 0x12, 0x60])
    assert "page-cross" not in src, src


def test_every_block_with_insns_is_charged():
    # BCS fork -> three blocks (entry, fall-through, taken), each non-empty,
    # so each must carry a cpu->cycles charge.
    src = _emit([
        0xB0, 0x02,  # BCS $8004
        0xEA, 0x60,  # NOP; RTS (fall-through)
        0xEA, 0x60,  # NOP; RTS (taken)
    ])
    charges = re.findall(r'cpu->cycles \+= (\d+);', src)
    assert len(charges) >= 3, f'expected a charge per block (>=3), got {charges}'


def test_master_constant_is_fetch_at_code_speed_plus_internal_at_6():
    # LDA #$05 (2: 2 fetch) ; RTS (6: 1 fetch, 2 data, 3 internal).
    src = _emit([0xA9, 0x05, 0x60])
    assert "cpu->cycles += 8;" in src, src
    assert f"cpu->master_cycles += 3 * {_SPEED} + 18;" in src, src
    # Every static cpu->cycles charge has exactly one master partner.
    cyc = _STATIC_CHARGE.findall(src)
    mas = _MASTER_CHARGE.findall(src)
    assert len(cyc) == len(mas), f'static charge pairing mismatch: {cyc} vs {mas}'


def test_wram_loop_block_prices_fetch_internal_and_data_apart():
    # InitNewGame's clear loop shape ($C0BDCE): STZ $1A6E,X / INX / CPX #$0100
    # / BNE, 16-bit index. 12 cycles: 9 fetches, 2 internal (STZ abs,X
    # indexing, INX), 1 data (the store, paced at its WRAM address). The old
    # constant charged all 12 at the code speed (FastROM 72 instead of 80).
    src = _emit([0x9E, 0x6E, 0x1A, 0xE8, 0xE0, 0x00, 0x01, 0xD0, 0xF7, 0x60], x=0)
    assert "cpu->cycles += 12;" in src, src
    assert f"cpu->master_cycles += 9 * {_SPEED} + 12;" in src, src
    assert "cpu_write8_paced(cpu, (uint8)(((((uint32)cpu->DB << 16) + (uint32)0x1a6e" in src


def test_data_accesses_are_paced_and_bookkeeping_is_not():
    # LDA $1234 ; PHA ; PLA ; JSR $9000 ; RTS
    rom = make_lorom_bank0({0x8000: bytes([0xAD, 0x34, 0x12, 0x48, 0x68,
                                           0x20, 0x00, 0x90, 0x60]),
                            0x9000: bytes([0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    body = _body(src)
    assert "cpu_read8_paced(cpu, cpu->DB, (uint16)(0x1234))" in body
    assert "cpu_write8_paced(cpu, 0x00, cpu->S, (uint8)(cpu->A & 0xFF))" in body
    assert body.count("cpu_write8_paced(cpu, 0x00, cpu->S, 0x80)") == 1   # JSR PCH
    # No data access of the body bypasses the paced bus.
    assert not re.search(r'\bcpu_(read|write)(8|16)\(', body), body
    # The prologue's peek at the caller's return frame stays unpaced.
    assert "_host_rpcl = cpu_read8(cpu, 0x00, (uint16)(_entry_s + 1u))" in src


def test_indirect_long_pointer_is_read_once():
    # LDA [$10],Y: the 24-bit pointer is read once (lo/hi word, then bank),
    # not once for the bank and again for the address expression.
    body = _body(_emit([0xB7, 0x10, 0x60]))
    assert body.count("cpu_read16_paced(cpu, 0x00, (uint16)(cpu->D + 0x0010))") == 1
    assert body.count("cpu_read8_paced(cpu, 0x00, (uint16)((uint16)(cpu->D + 0x0010) + 2))") == 1
    assert "(((uint32)_ib1 << 16) | (uint32)_ip1) + (uint32)cpu->Y" in body


def test_dp_indirect_y_read_page_cross_is_charged_at_index_width_8():
    src8 = _emit([0xB1, 0x10, 0x60], x=1)       # LDA ($10),Y
    assert ("if ((_ip1 & 0xFF00) != ((uint16)(_ip1 + cpu->Y) & 0xFF00)) "
            "{ cpu->cycles += 1; cpu->master_cycles += 6; }") in src8, src8
    # At index width 16 the indexing cycle is always taken: static, no check.
    src16 = _emit([0xB1, 0x10, 0x60], x=0)
    assert "(dp),Y read page-cross" not in src16
    # A store pays the indexing cycle in its base.
    assert "(dp),Y read page-cross" not in _emit([0x91, 0x10, 0x60], x=1)


def test_mdmaen_store_ends_the_charge_segment():
    # STA $420B ; NOP ; RTS: after the DMA the frame deadline is checked and
    # the rest of the block is charged only if the block goes on.
    src = _emit([0x8D, 0x0B, 0x42, 0xEA, 0x60])
    store = src.index("cpu_write8_paced(cpu, cpu->DB, (uint16)(0x420b)")
    check = src.index("/* MDMAEN store: the DMA ran; end the frame here if due */")
    assert store < check
    tail = src[check:]
    assert "return interp_bridge_lle_yield_unwind(cpu, (((uint32)cpu->PB << 16) | 0x8003u));" in tail
    # Two segments: STA (4 cycles: 3 fetch, 1 data) and NOP + RTS (8 cycles).
    assert _STATIC_CHARGE.findall(src) == ['4', '8'], _STATIC_CHARGE.findall(src)
    assert f"cpu->master_cycles += 3 * {_SPEED};" in src[:check]
    assert f"cpu->master_cycles += 2 * {_SPEED} + 24;" in tail


def test_mdmaen_long_and_word_stores_split_too():
    # sta f:$00420B (long) and a 16-bit sta $420A (writes $420A and $420B).
    assert "MDMAEN store" in _emit([0x8F, 0x0B, 0x42, 0x00, 0xEA, 0x60])
    assert "MDMAEN store" in _emit([0x8D, 0x0A, 0x42, 0xEA, 0x60], m=0)
    # An 8-bit store to $420A and a long store into WRAM do not split.
    assert "MDMAEN store" not in _emit([0x8D, 0x0A, 0x42, 0xEA, 0x60], m=1)
    assert "MDMAEN store" not in _emit([0x8F, 0x0B, 0x42, 0x7E, 0xEA, 0x60])
