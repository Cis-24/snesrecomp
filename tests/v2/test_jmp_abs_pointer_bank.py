"""Regression tests: `JMP (abs)` fetches its pointer from bank 0, not PB.

On the 65816, opcode $6C reads the 16-bit pointer at 00:abs; only the jump
target lands in PB. Code in bank 0 cannot tell the two apart, so these tests
run in HiROM bank C0. FF6's event dispatcher (`jmp ($002a)` at C0:9B59) keeps
its handler pointer in WRAM $002A, while ROM C0:002A holds unrelated bytes
($AB48); reading PB:$002A sent every native event dispatch to C0:AB48.
"""
from _helpers import REPO  # noqa: F401,E402  (sets up sys.path)

from snes65816 import ROM_MAP_HIROM, get_rom_mapping, set_rom_mapping  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


def _hirom_bank_c0(blobs: dict) -> bytes:
    """64KB image of HiROM bank C0 with the FF6 decoy word at C0:002A."""
    rom = bytearray(0x10000)
    rom[0x002A:0x002C] = bytes([0x48, 0xAB])
    for pc, blob in blobs.items():
        rom[pc:pc + len(blob)] = blob
    return bytes(rom)


def _emit_in_c0(rom: bytes, **kwargs) -> str:
    old_mapping = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_HIROM)
        return emit_function(rom=rom, bank=0xC0, **kwargs)
    finally:
        set_rom_mapping(old_mapping)


def test_value_pointer_dispatch_in_bank_c0_reads_pointer_from_bank0():
    rom = _hirom_bank_c0({
        0x9B50: bytes([
            0xC2, 0x20,        # REP #$20
            0xBF, 0x00, 0xA0, 0xC0,  # LDA $C0A000,X ; handler pointer
            0x85, 0x2A,        # STA $2a
            0x6C, 0x2A, 0x00,  # JMP ($002a)
        ]),
        0xA000: bytes([
            0x00, 0x91,
            0x00, 0x92,
        ]),
        0x9100: bytes([0x60]),  # RTS
        0x9200: bytes([0x60]),  # RTS
    })

    src = _emit_in_c0(
        rom,
        start=0x9B50,
        entry_m=1,
        entry_x=1,
        func_name='EventDispatchC0',
        indirect_dispatch={
            0xC09B58: {
                'count': 2,
                'idx_reg': 'X',
                'table_bases': (0xA000,),
            },
        },
    )

    assert 'absolute indirect dispatch: switch on the loaded pointer' in src
    assert 'uint16 _target = cpu_read16(cpu, 0x00, (uint16)0x002a)' in src
    assert 'cpu_read16(cpu, cpu->PB, (uint16)0x002a)' not in src
    assert 'case 0x9100:' in src
    assert 'case 0x9200:' in src
    # The fallback still jumps within PB.
    assert '((uint32)cpu->PB << 16) | _target' in src


def test_local_goto_dispatch_in_bank_c0_reads_pointer_from_bank0():
    rom = _hirom_bank_c0({
        0x8000: bytes([
            0x4A,              # LSR A
            0x85, 0x12,        # STA $12
            0x4A,              # LSR A
            0x65, 0x12,        # ADC $12
            0x18,              # CLC
            0x69, 0x20, 0x80,  # ADC #$8020
            0x85, 0x12,        # STA $12
            0xA9, 0xF0, 0x00,  # LDA #$00F0
            0xE2, 0x30,        # SEP #$30
            0x6C, 0x12, 0x00,  # JMP ($0012)
        ]),
        0x8020: bytes([
            0x8D, 0x70, 0x03,  # STA $0370
            0x8D, 0x74, 0x03,  # STA $0374
            0x8D, 0x78, 0x03,  # STA $0378
            0x8D, 0x7C, 0x03,  # STA $037C
            0x60,              # RTS
        ]),
    })

    src = _emit_in_c0(
        rom,
        start=0x8000,
        entry_m=0,
        entry_x=0,
        end=0x802D,
        func_name='LocalStrideRunwayC0',
    )

    assert 'local computed-goto dispatch' in src
    assert 'uint16 _target = cpu_read16(cpu, 0x00, (uint16)0x0012)' in src
    assert 'cpu_read16(cpu, cpu->PB, (uint16)0x0012)' not in src
    assert 'case 0x8020: goto L_8020_M1X1;' in src
    assert 'case 0x8029: goto L_8029_M1X1;' in src
