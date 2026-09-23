#!/usr/bin/env python3
"""
Harvest Moon 64 Dialogue Message Extractor.

Extracts and decodes dialogue message banks from the Harvest Moon 64 ROM
(ROM offsets 0x00E871A0 - 0x00E93080), as registered in the engine's
message dispatch table (func_80043050).
"""

import argparse
import json
from pathlib import Path
import struct
import sys

# Default ROM path
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = REPO_ROOT / "roms" / "harvest-moon-64.z64"

# Dialogue banks registered by func_80054734 (Bank ID, text_start, tbl_start, tbl_end)
DIALOGUE_BANKS = [
    (0, 0x00E871A0, 0x00E871B0, 0x00E871C0),
    (1, 0x00E871C0, 0x00E872C0, 0x00E87320),
    (2, 0x00E87320, 0x00E87BB0, 0x00E87CE0),
    (3, 0x00E87CE0, 0x00E88190, 0x00E88240),
    (4, 0x00E88240, 0x00E88B20, 0x00E88C60),
    (5, 0x00E88C60, 0x00E89070, 0x00E89120),
    (6, 0x00E89120, 0x00E89600, 0x00E896B0),
    (7, 0x00E896B0, 0x00E89B60, 0x00E89C00),
    (8, 0x00E89C00, 0x00E8A460, 0x00E8A590),
    (9, 0x00E8A590, 0x00E8AE00, 0x00E8AF30),
    (10, 0x00E8AF30, 0x00E8B7A0, 0x00E8B8F0),
    (11, 0x00E8B8F0, 0x00E8BCE0, 0x00E8BD80),
    (12, 0x00E8BD80, 0x00E8C000, 0x00E8C070),
    (13, 0x00E8C070, 0x00E8C400, 0x00E8C480),
    (14, 0x00E8C480, 0x00E8C750, 0x00E8C7D0),
    (15, 0x00E8C7D0, 0x00E8CBA0, 0x00E8CC40),
    (16, 0x00E8CC40, 0x00E8CF90, 0x00E8D010),
    (17, 0x00E8D010, 0x00E8D3F0, 0x00E8D470),
    (18, 0x00E8D470, 0x00E8D7B0, 0x00E8D830),
    (19, 0x00E8D830, 0x00E8DA90, 0x00E8DB00),
    (20, 0x00E8DB00, 0x00E8DDF0, 0x00E8DE60),
    (21, 0x00E8DE60, 0x00E8E0E0, 0x00E8E160),
    (22, 0x00E8E160, 0x00E8E490, 0x00E8E520),
    (23, 0x00E8E520, 0x00E8E780, 0x00E8E7F0),
    (24, 0x00E8E7F0, 0x00E8EAC0, 0x00E8EB30),
    (25, 0x00E8EB30, 0x00E8EE50, 0x00E8EEC0),
    (26, 0x00E8EEC0, 0x00E8F1E0, 0x00E8F250),
    (27, 0x00E8F250, 0x00E8F550, 0x00E8F5C0),
    (28, 0x00E8F5C0, 0x00E8F7A0, 0x00E8F810),
    (29, 0x00E8F810, 0x00E8FA00, 0x00E8FA70),
    (30, 0x00E8FA70, 0x00E8FC90, 0x00E8FD00),
    (31, 0x00E8FD00, 0x00E90000, 0x00E90080),
    (32, 0x00E90080, 0x00E903D0, 0x00E90460),
    (33, 0x00E90460, 0x00E90810, 0x00E90890),
    (34, 0x00E90890, 0x00E90B00, 0x00E90B70),
    (35, 0x00E90B70, 0x00E90DD0, 0x00E90E40),
    (36, 0x00E90E40, 0x00E91210, 0x00E91290),
    (37, 0x00E91290, 0x00E914C0, 0x00E91520),
    (38, 0x00E91520, 0x00E917E0, 0x00E91860),
    (39, 0x00E91860, 0x00E918A0, 0x00E918D0),
    (40, 0x00E918D0, 0x00E91900, 0x00E91920),
    (41, 0x00E91920, 0x00E91970, 0x00E919A0),
    (42, 0x00E919A0, 0x00E91A50, 0x00E91AB0),
    (43, 0x00E91AB0, 0x00E91AD0, 0x00E91AE0),
    (44, 0x00E91AE0, 0x00E91B50, 0x00E91B80),
    (45, 0x00E91B80, 0x00E91B90, 0x00E91BA0),
    (46, 0x00E91BA0, 0x00E91BF0, 0x00E91C20),
    (47, 0x00E91C20, 0x00E91CE0, 0x00E91D30),
    (48, 0x00E91D30, 0x00E91DC0, 0x00E91E00),
    (49, 0x00E91E00, 0x00E91EF0, 0x00E91F60),
    (50, 0x00E91F60, 0x00E91F90, 0x00E91FB0),
    (51, 0x00E91FB0, 0x00E91FF0, 0x00E92010),
    (52, 0x00E92010, 0x00E92030, 0x00E92040),
    (53, 0x00E92040, 0x00E92180, 0x00E92220),
    (54, 0x00E92220, 0x00E922E0, 0x00E92370),
    (55, 0x00E92370, 0x00E92380, 0x00E92390),
    (56, 0x00E92390, 0x00E924A0, 0x00E92520),
    (57, 0x00E92520, 0x00E92550, 0x00E92570),
    (58, 0x00E92570, 0x00E925B0, 0x00E925F0),
    (59, 0x00E925F0, 0x00E92600, 0x00E92610),
    (60, 0x00E92610, 0x00E92630, 0x00E92640),
    (61, 0x00E92640, 0x00E92800, 0x00E928B0),
    (62, 0x00E928B0, 0x00E92910, 0x00E92950),
    (63, 0x00E92950, 0x00E92B40, 0x00E92B90),
    (64, 0x00E92B90, 0x00E92C50, 0x00E92C70),
    (65, 0x00E92C70, 0x00E92DB0, 0x00E92DE0),
    (66, 0x00E92DE0, 0x00E92E90, 0x00E92EC0),
    (67, 0x00E92EC0, 0x00E92EE0, 0x00E92EF0),
    (68, 0x00E92EF0, 0x00E92F90, 0x00E92FF0),
    (69, 0x00E92FF0, 0x00E93060, 0x00E93080),
]


def decode_message_stream(raw_bytes: bytes) -> str:
    """Decode raw message bytecode stream into human-readable tokens."""
    tokens = []
    idx = 0
    n = len(raw_bytes)

    while idx < n:
        b = raw_bytes[idx]

        # Escape code / control sequence
        if b == 0xFF:
            if idx + 1 < n:
                code = raw_bytes[idx + 1]
                idx += 2
                # Known control codes from messageDecodeControlCode (func_80043EC8)
                if code == 0x00 and idx + 2 <= n:
                    arg = struct.unpack(">H", raw_bytes[idx : idx + 2])[0]
                    tokens.append(f"<WAIT:{arg}>")
                    idx += 2
                elif code == 0x01 and idx + 1 <= n:
                    arg = raw_bytes[idx]
                    tokens.append(f"<CTRL_1:{arg}>")
                    idx += 1
                elif code == 0x02 and idx + 3 <= n:
                    arg = raw_bytes[idx]
                    val = struct.unpack(">H", raw_bytes[idx + 1 : idx + 3])[0]
                    tokens.append(f"<COLOR:{arg},{val}>")
                    idx += 3
                elif code == 0x0A and idx + 1 <= n:
                    arg = raw_bytes[idx]
                    tokens.append(f"<BOX_STYLE:{arg}>")
                    idx += 1
                elif code == 0x0C and idx + 2 <= n:
                    val = struct.unpack(">H", raw_bytes[idx : idx + 2])[0]
                    tokens.append(f"<PARAM:{val}>")
                    idx += 2
                elif code == 0xFF and idx + 1 <= n:
                    ext_code = raw_bytes[idx]
                    idx += 1
                    tokens.append(f"<ESC:{ext_code:02X}>")
                else:
                    tokens.append(f"<CTRL:{code:02X}>")
            else:
                tokens.append("<FF>")
                idx += 1
        elif b == 0x0C:
            tokens.append("<NL>\n")
            idx += 1
        elif b == 0x0F:
            tokens.append(" ")
            idx += 1
        else:
            tokens.append(f"[{b:02X}]")
            idx += 1

    return "".join(tokens)


def dump_bank(rom: bytes, bank_id: int, text_start: int, tbl_start: int, tbl_end: int):
    """Dump all dialogue messages in a single bank."""
    num_messages = (tbl_end - tbl_start) // 4
    offsets = struct.unpack(f">{num_messages}I", rom[tbl_start:tbl_end])

    messages = []
    for i in range(num_messages):
        rel_start = offsets[i]
        rel_end = offsets[i + 1] if i + 1 < num_messages else (tbl_start - text_start)
        abs_start = text_start + rel_start
        abs_end = text_start + rel_end
        raw_msg = rom[abs_start:abs_end]

        decoded = decode_message_stream(raw_msg)
        messages.append({
            "index": i,
            "rom_offset": f"0x{abs_start:06X}",
            "size": len(raw_msg),
            "raw_hex": raw_msg.hex(),
            "decoded": decoded,
        })

    return {
        "bank_id": bank_id,
        "text_start": f"0x{text_start:06X}",
        "tbl_start": f"0x{tbl_start:06X}",
        "tbl_end": f"0x{tbl_end:06X}",
        "message_count": num_messages,
        "messages": messages,
    }


def main():
    parser = argparse.ArgumentParser(description="Harvest Moon 64 Dialogue Dump Tool")
    parser.add_argument("--rom", type=Path, default=DEFAULT_ROM, help="Path to input ROM")
    parser.add_argument("--bank", type=int, default=2, help="Specific bank ID to dump (default: 2, at 0x00E87320)")
    parser.add_argument("--all", action="store_true", help="Dump all 70 dialogue banks")
    parser.add_argument("--format", choices=["text", "json"], default="text", help="Output format")
    parser.add_argument("-o", "--out", type=Path, help="Output file path (default: stdout)")

    args = parser.parse_args()

    if not args.rom.exists():
        print(f"Error: ROM not found at {args.rom}", file=sys.stderr)
        sys.exit(1)

    with open(args.rom, "rb") as f:
        rom = f.read()

    target_banks = DIALOGUE_BANKS if args.all else [b for b in DIALOGUE_BANKS if b[0] == args.bank]
    if not target_banks:
        print(f"Error: Bank ID {args.bank} not found (available: 0..69)", file=sys.stderr)
        sys.exit(1)

    results = [dump_bank(rom, *b) for b in target_banks]

    if args.format == "json":
        out_text = json.dumps(results if args.all else results[0], indent=2)
    else:
        lines = []
        for b in results:
            lines.append("=" * 70)
            lines.append(f"BANK {b['bank_id']:02d}: {b['message_count']} messages (ROM {b['text_start']}..{b['tbl_start']})")
            lines.append("=" * 70)
            for m in b["messages"]:
                lines.append(f"Message {m['index']:03d} [{m['rom_offset']}, size {m['size']} bytes]:")
                lines.append(f"  Raw:     {m['raw_hex'][:64]}{'...' if len(m['raw_hex']) > 64 else ''}")
                lines.append(f"  Decoded: {m['decoded']}")
                lines.append("")
        out_text = "\n".join(lines)

    if args.out:
        args.out.write_text(out_text, encoding="utf-8")
        print(f"Dump written to {args.out}")
    else:
        print(out_text)


if __name__ == "__main__":
    main()
