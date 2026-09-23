#!/usr/bin/env python3
"""
Harvest Moon 64 Dialogue & Message Extractor.

Harvest Moon 64 organizes dialogue into two distinct layers:
1. Dialogue Text Banks (ROM 0x00E13800 - 0x00E871A0):
   68 dialogue text banks registered by func_800535DC via func_8003F30C.
   Text is encoded in a variable-width bitstream (func_80041850 in message.c)
   using the game's custom font table (0xAC..0xC5 = A..Z, 0xC6..0xDF = a..z,
   0xE0..0xE9 = 0..9, 0xF9 = space, 0xF0 = '.', 0xEF = ',', 0xEA = '?', 0xF5 = '!',
   0xEB = '-', 0x010E = "'").
   Line breaks (0x00) and page breaks (0x04) are embedded directly in the stream.

2. Dialogue Script / Dispatch Banks (ROM 0x00E871A0 - 0x00E93080):
   70 engine banks registered by func_80054734 into D_80205760 (handled by
   message.c). These contain bytecode for text box styling, conditional
   variable checks, camera / sprite triggers, and dialogue branching.
"""

import argparse
import json
from pathlib import Path
import struct
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = REPO_ROOT / "roms" / "harvest-moon-64.z64"

# Custom font table for Harvest Moon 64:
# 0xAC..0xC5 is Uppercase 'A'..'Z'
# 0xC6..0xDF is Lowercase 'a'..'z'
# 0xE0..0xE9 is Digits '0'..'9'
FONT_TABLE = {}
for i in range(26):
    FONT_TABLE[0xAC + i] = chr(ord("A") + i)
    FONT_TABLE[0xC6 + i] = chr(ord("a") + i)
# Digits are ordered 1..9, 0 (as on a standard keyboard / JIS layout)
for i, d in enumerate("1234567890"):
    FONT_TABLE[0xE0 + i] = d

FONT_TABLE[0xF9] = " "
FONT_TABLE[0xF0] = "."
FONT_TABLE[0xEF] = ","
FONT_TABLE[0xEA] = "?"
FONT_TABLE[0xF5] = "!"
FONT_TABLE[0xEB] = "-"
FONT_TABLE[0xEE] = ":"
FONT_TABLE[0xED] = "/"
FONT_TABLE[0xF4] = " & "
FONT_TABLE[0xF1] = " & "

# Text pool boundaries in ROM
TEXT_POOL_START = 0x00E13800
TEXT_POOL_END = 0x00E871A0

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



# 68 contiguous unique text dialogue banks registered by func_800535DC via func_8003F30C
# (Bank ID, text_start, tbl_start, tbl_end)
TEXT_BANKS = [
    (0, 0x00E13800, 0x00E13920, 0x00E13990),
    (1, 0x00E13990, 0x00E13C30, 0x00E13C60),
    (2, 0x00E13C60, 0x00E167C0, 0x00E16A90),
    (3, 0x00E16A90, 0x00E19F60, 0x00E19FF0),
    (4, 0x00E19FF0, 0x00E1B3A0, 0x00E1B4E0),
    (5, 0x00E1B4E0, 0x00E1C8C0, 0x00E1C9C0),
    (6, 0x00E1C9C0, 0x00E21150, 0x00E21700),
    (7, 0x00E21700, 0x00E218A0, 0x00E218D0),
    (8, 0x00E218D0, 0x00E24E20, 0x00E24F60),
    (9, 0x00E24F60, 0x00E24F70, 0x00E24F80),
    (10, 0x00E24F80, 0x00E25170, 0x00E251D0),
    (11, 0x00E251D0, 0x00E28460, 0x00E28720),
    (12, 0x00E28720, 0x00E2A230, 0x00E2A380),
    (13, 0x00E2A380, 0x00E2D080, 0x00E2D330),
    (14, 0x00E2D330, 0x00E2E500, 0x00E2E600),
    (15, 0x00E2E600, 0x00E2F650, 0x00E2F730),
    (16, 0x00E2F730, 0x00E30ED0, 0x00E31010),
    (17, 0x00E31010, 0x00E33020, 0x00E33190),
    (18, 0x00E33190, 0x00E34650, 0x00E34790),
    (19, 0x00E34790, 0x00E36000, 0x00E36150),
    (20, 0x00E36150, 0x00E39360, 0x00E39610),
    (21, 0x00E39610, 0x00E3C700, 0x00E3C9A0),
    (22, 0x00E3C9A0, 0x00E3D600, 0x00E3D6D0),
    (23, 0x00E3D6D0, 0x00E40B90, 0x00E40E50),
    (24, 0x00E40E50, 0x00E42010, 0x00E42100),
    (25, 0x00E42100, 0x00E42D50, 0x00E42E60),
    (26, 0x00E42E60, 0x00E43E80, 0x00E43F60),
    (27, 0x00E43F60, 0x00E45130, 0x00E45200),
    (28, 0x00E45200, 0x00E461C0, 0x00E46280),
    (29, 0x00E46280, 0x00E47140, 0x00E471E0),
    (32, 0x00E49540, 0x00E4A230, 0x00E4A2D0),
    (33, 0x00E4A2D0, 0x00E4B6E0, 0x00E4B7D0),
    (34, 0x00E4B7D0, 0x00E4C8D0, 0x00E4C9B0),
    (35, 0x00E4C9B0, 0x00E4E230, 0x00E4E320),
    (36, 0x00E4E320, 0x00E4EC70, 0x00E4ED10),
    (37, 0x00E4ED10, 0x00E4F370, 0x00E4F3F0),
    (38, 0x00E4F3F0, 0x00E50520, 0x00E50600),
    (39, 0x00E50600, 0x00E51A00, 0x00E51AE0),
    (40, 0x00E51AE0, 0x00E52740, 0x00E527E0),
    (41, 0x00E527E0, 0x00E53450, 0x00E53540),
    (42, 0x00E53540, 0x00E54C60, 0x00E54D60),
    (43, 0x00E54D60, 0x00E55AE0, 0x00E55BA0),
    (44, 0x00E55BA0, 0x00E569C0, 0x00E56A80),
    (45, 0x00E56A80, 0x00E58010, 0x00E58160),
    (46, 0x00E58160, 0x00E583C0, 0x00E583F0),
    (47, 0x00E583F0, 0x00E58880, 0x00E588D0),
    (48, 0x00E588D0, 0x00E5A6D0, 0x00E5A8C0),
    (49, 0x00E5A8C0, 0x00E5D240, 0x00E5D4C0),
    (50, 0x00E5D4C0, 0x00E5E270, 0x00E5E340),
    (51, 0x00E5E340, 0x00E5F220, 0x00E5F300),
    (52, 0x00E5F300, 0x00E60080, 0x00E60170),
    (53, 0x00E60170, 0x00E64680, 0x00E64AF0),
    (54, 0x00E64AF0, 0x00E66260, 0x00E663F0),
    (55, 0x00E663F0, 0x00E677B0, 0x00E678E0),
    (56, 0x00E678E0, 0x00E68620, 0x00E68700),
    (57, 0x00E68700, 0x00E68BA0, 0x00E68BF0),
    (58, 0x00E68BF0, 0x00E6ADC0, 0x00E6AFE0),
    (59, 0x00E6AFE0, 0x00E6F1E0, 0x00E6F5C0),
    (60, 0x00E6F5C0, 0x00E714D0, 0x00E71690),
    (61, 0x00E71690, 0x00E72680, 0x00E72780),
    (62, 0x00E72780, 0x00E73070, 0x00E73110),
    (63, 0x00E73110, 0x00E7C7B0, 0x00E7D1E0),
    (64, 0x00E7D1E0, 0x00E81660, 0x00E81F50),
    (65, 0x00E81F50, 0x00E82B30, 0x00E82C40),
    (66, 0x00E82C40, 0x00E83880, 0x00E83960),
    (67, 0x00E83960, 0x00E84690, 0x00E84760),
    (72, 0x00E857A0, 0x00E87080, 0x00E871A0),
]


class TextDecoder:
    """
    Decodes Harvest Moon 64 variable-length text bitstreams according to func_80041850 (message.c).

    The stream contains 1-byte flag headers every 8 characters. Bit 0 indicates a
    single-byte font code; bit 1 indicates a 2-byte font code.
    """

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0
        self.bit_count = 0
        self.flags = 0

    def next_code(self) -> int | None:
        if (self.bit_count & 7) == 0:
            if self.pos >= len(self.data):
                return None
            self.flags = self.data[self.pos]
            self.pos += 1
        mask = 1 << (7 - (self.bit_count & 7))
        self.bit_count += 1
        if self.flags & mask:
            if self.pos + 1 >= len(self.data):
                return None
            b1 = self.data[self.pos]
            b2 = self.data[self.pos + 1]
            self.pos += 2
            return (b2 << 8) | b1
        else:
            if self.pos >= len(self.data):
                return None
            b = self.data[self.pos]
            self.pos += 1
            return b


def decode_dialogue(raw_bytes: bytes) -> str:
    """
    Decode dialogue bytecode according to message.c (func_80040C38).
    Preserves multi-line layout (0x00 newline) and multi-page layout (0x04 page break).
    """
    dec = TextDecoder(raw_bytes)
    res = []
    while True:
        code = dec.next_code()
        if code is None or code == 0x02:
            break
        if code == 0x09:
            # Opcode 0x09 (dialogue box style) consumes 1 raw byte from the stream
            if dec.pos < len(dec.data):
                dec.pos += 1
        elif code == 0x00:
            res.append("\n")
        elif code == 0x04:
            res.append("\n---\n")
        elif code in (0x010E, 0x0E01):
            res.append("'")
        elif code in FONT_TABLE:
            res.append(FONT_TABLE[code])
        elif 0x20 <= code <= 0x7E:
            res.append(chr(code))

    # Format pages cleanly, preserving line breaks and page breaks
    raw_text = "".join(res)
    pages = [p.strip() for p in raw_text.split("---")]
    formatted_pages = []
    for p in pages:
        lines = [line.strip() for line in p.splitlines() if line.strip()]
        if lines:
            formatted_pages.append("\n".join(lines))
    return "\n---\n".join(formatted_pages)


def dump_text_pool(
    rom: bytes, max_entries: int = 50, bank_id: int | None = None
) -> list[dict]:
    """
    Extract readable dialogue messages from the ROM dialogue text banks
    using the native bitstream decoder (func_80041850).
    """
    banks = (
        [b for b in TEXT_BANKS if b[0] == bank_id]
        if bank_id is not None
        else TEXT_BANKS
    )
    extracted = []

    for b in banks:
        b_idx, text_start, tbl_start, tbl_end = b
        num_msg = (tbl_end - tbl_start) // 4
        for i in range(num_msg):
            o1 = struct.unpack(">I", rom[tbl_start + i * 4 : tbl_start + (i + 1) * 4])[0]
            o2 = (
                struct.unpack(
                    ">I", rom[tbl_start + (i + 1) * 4 : tbl_start + (i + 2) * 4]
                )[0]
                if i + 1 < num_msg
                else (tbl_start - text_start)
            )
            if o2 <= o1:
                continue
            raw = rom[text_start + o1 : text_start + o2]
            msg = decode_dialogue(raw)
            if msg and len([c for c in msg if c.isalpha()]) >= 4:
                extracted.append({
                    "bank_id": b_idx,
                    "msg_id": i,
                    "rom_offset": f"0x{text_start + o1:06X}",
                    "text": msg,
                })
                if max_entries and len(extracted) >= max_entries:
                    return extracted

    return extracted




def decode_script_bytecode(raw_bytes: bytes) -> str:
    """Decode raw dialogue bank bytecode stream into human-readable tokens."""
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
                if code == 0x00 and idx + 2 <= n:
                    arg = struct.unpack(">H", raw_bytes[idx : idx + 2])[0]
                    tokens.append(f"<WAIT:{arg}>")
                    idx += 2
                elif code == 0x01 and idx + 1 <= n:
                    arg = raw_bytes[idx]
                    tokens.append(f"<VAR_CHECK:{arg}>")
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
        elif b in FONT_TABLE:
            tokens.append(FONT_TABLE[b])
        else:
            tokens.append(f"[{b:02X}]")
            idx += 1

    return "".join(tokens)


def dump_bank(rom: bytes, bank_id: int, text_start: int, tbl_start: int, tbl_end: int):
    """Dump all dialogue script entries in a single dispatch bank."""
    num_messages = (tbl_end - tbl_start) // 4
    offsets = struct.unpack(f">{num_messages}I", rom[tbl_start:tbl_end])

    messages = []
    for i in range(num_messages):
        rel_start = offsets[i]
        rel_end = offsets[i + 1] if i + 1 < num_messages else (tbl_start - text_start)
        abs_start = text_start + rel_start
        abs_end = text_start + rel_end
        raw_msg = rom[abs_start:abs_end]

        decoded = decode_script_bytecode(raw_msg)
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
    parser = argparse.ArgumentParser(description="Harvest Moon 64 Dialogue & Message Extractor")
    parser.add_argument("--rom", type=Path, default=DEFAULT_ROM, help="Path to input ROM")
    parser.add_argument("--dialogue", action="store_true", help="Dump human-readable English dialogue text banks (ROM 0x00E13800)")
    parser.add_argument("--limit", type=int, default=50, help="Number of dialogue lines to dump in --dialogue mode (default: 50, 0 for all)")
    parser.add_argument("--bank", type=int, default=None, help="Specific bank ID to dump (default: bank 2 for script banks)")
    parser.add_argument("--all", action="store_true", help="Dump all 70 dispatch banks")
    parser.add_argument("--format", choices=["text", "json"], default="text", help="Output format")
    parser.add_argument("-o", "--out", type=Path, help="Output file path (default: stdout)")

    args = parser.parse_args()

    if not args.rom.exists():
        print(f"Error: ROM not found at {args.rom}", file=sys.stderr)
        sys.exit(1)

    with open(args.rom, "rb") as f:
        rom = f.read()

    if args.dialogue:
        entries = dump_text_pool(rom, max_entries=args.limit, bank_id=args.bank)
        if args.format == "json":
            out_text = json.dumps({"dialogue_boxes": entries}, indent=2)
        else:
            lines = [
                "=" * 70,
                f"HARVEST MOON 64 DIALOGUE TEXT CORPUS (ROM 0x{TEXT_POOL_START:06X} - 0x{TEXT_POOL_END:06X})",
                "=" * 70,
            ]
            for idx, entry in enumerate(entries, start=1):
                lines.append(f"[{idx:04d}] (Bank {entry['bank_id']} Msg {entry['msg_id']})")
                for line in entry["text"].split("\n"):
                    lines.append(f"  {line}")
                lines.append("")
            out_text = "\n".join(lines).rstrip() + "\n"
    else:
        bank_id = args.bank if args.bank is not None else 2
        target_banks = DIALOGUE_BANKS if args.all else [b for b in DIALOGUE_BANKS if b[0] == bank_id]
        if not target_banks:
            print(f"Error: Bank ID {bank_id} not found (available: 0..69)", file=sys.stderr)
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
