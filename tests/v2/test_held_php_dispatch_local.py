"""Regression tests: a JMP (abs,X) tail dispatch made while the path holds a
PHP keeps its targets inside the function.

FF6's ValidateSpellList (C2:568D) saves X and P on entry with X=1, switches
to 16-bit X and loops over `JMP (AddToSpellListTbl,X)`. Every target ends in
the shared tail `DEY4; BNE loop; PLP; PLX; RTS`. Emitted as tail calls into
separate functions, each target started with an empty PHP history, so its PLP
kept the dispatch-site width X=0 and the PLX pulled 16 bits although the PLP
had restored X=1. S came out one byte off and the RTS jumped into the zero
page. A plain jump onto a sibling entry was already decoded through in this
situation (the sibling gate); the dispatch now does the same.

The code sits below $8000 in a HiROM bank, as in FF6.
"""
from _helpers import REPO  # noqa: F401,E402  (sets up sys.path)

from snes65816 import ROM_MAP_HIROM, get_rom_mapping, set_rom_mapping  # noqa: E402
from v2.decoder import DecodeKey, addr24, decode_function  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402

BANK = 0xC0
START = 0x5680
SITE = 0x568A
TAIL = 0x568D
PLX = 0x5691
TARGETS = (0x5710, 0x5718)


def _rom(with_php: bool) -> bytes:
    """64KB image of HiROM bank C0 with the ValidateSpellList shape."""
    rom = bytearray(0x10000)
    blobs = {
        START: bytes([
            0xDA,              # PHX           (entry X=1: one byte)
            0x08 if with_php else 0xEA,  # PHP (or NOP)
            0xC2, 0x10,        # REP #$10      (16-bit X)
            0xA2, 0x00, 0x00,  # LDX #$0000
            0xA0, 0x02, 0x00,  # LDY #$0002
            0x7C, 0x00, 0x57,  # JMP ($5700,X)
            0x88,              # TAIL: DEY
            0xD0, 0xFA,        # BNE SITE
            0x28 if with_php else 0xEA,  # PLP (or NOP)
            0xFA,              # PLX
            0x60,              # RTS
        ]),
        0x5700: bytes([0x10, 0x57, 0x18, 0x57]),       # table
        0x5710: bytes([0xA9, 0x01, 0x4C, 0x8D, 0x56]),  # LDA #1; JMP TAIL
        0x5718: bytes([0xA9, 0x02, 0x4C, 0x8D, 0x56]),  # LDA #2; JMP TAIL
    }
    for pc, blob in blobs.items():
        rom[pc:pc + len(blob)] = blob
    return bytes(rom)


_DISPATCH = {
    addr24(BANK, SITE): {'count': 2, 'idx_reg': 'X', 'table_bases': ()},
}


def _in_hirom(fn, *args, **kwargs):
    old_mapping = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_HIROM)
        return fn(*args, **kwargs)
    finally:
        set_rom_mapping(old_mapping)


def _decode(with_php: bool):
    return _in_hirom(
        decode_function, _rom(with_php), bank=BANK, start=START,
        entry_m=1, entry_x=1, indirect_dispatch=_DISPATCH,
        sibling_entry_pcs={0x5710, 0x5718})


def _emit(with_php: bool) -> str:
    return _in_hirom(
        emit_function, rom=_rom(with_php), bank=BANK, start=START,
        entry_m=1, entry_x=1, func_name='ValidateSpellListShape',
        indirect_dispatch=_DISPATCH, sibling_entry_pcs={0x5710, 0x5718})


def _keys_at(graph, pc16):
    return [k for k in graph.insns if k.pc == addr24(BANK, pc16)]


def test_dispatch_under_php_decodes_targets_with_the_php_history():
    graph = _decode(with_php=True)
    site = _keys_at(graph, SITE)
    assert site, "dispatch site not decoded"
    insn = graph.insns[site[0]].insn
    assert insn.dispatch_local_goto is True
    for target in TARGETS:
        key = DecodeKey(addr24(BANK, target), 1, 0, ((1, 1),))
        assert key in graph.insns, f"target ${target:04X} not decoded locally"


def test_plp_after_local_dispatch_restores_the_pushed_width():
    graph = _decode(with_php=True)
    plx = _keys_at(graph, PLX)
    assert plx, "PLX not decoded"
    assert {(k.m, k.x) for k in plx} == {(1, 1)}, (
        "PLX must decode at the width the entry PHP saved (X=1)")


def test_dispatch_without_php_keeps_tail_calls():
    graph = _decode(with_php=False)
    insn = graph.insns[_keys_at(graph, SITE)[0]].insn
    assert insn.dispatch_local_goto is False


def test_codegen_switches_on_the_indexed_pointer_and_pulls_one_byte():
    src = _emit(with_php=True)
    assert ('uint16 _target = cpu_read16(cpu, cpu->PB, '
            '(uint16)(0x5700 + (cpu->X & 0xFFFF)))') in src
    assert 'case 0x5710: goto L_5710_M1X0;' in src
    assert 'case 0x5718: goto L_5718_M1X0;' in src
    assert 'bank_C0_5710' not in src
    assert 'bank_C0_5718' not in src
    assert 'CPU_STACK_OP_PLX, _old_s, +1' in src
    assert 'CPU_STACK_OP_PLX, _old_s, +2' not in src


def test_codegen_without_php_still_tail_calls_the_targets():
    src = _emit(with_php=False)
    assert 'local computed-goto dispatch' not in src
    assert 'bank_C0_5710' in src
