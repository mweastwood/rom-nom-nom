#!/usr/bin/env python3
"""
Simple and robust BPS (Beat Patch System) patch generator and applier.
Specification: https://www.romhacking.net/documents/746/
"""

import os
import sys
import zlib
import struct
from pathlib import Path


def encode_vli(value: int) -> bytes:
    """Encode an integer as a BPS variable-length integer."""
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(b | 0x80)
            break
        out.append(b)
        value -= 1
    return bytes(out)


def decode_vli(data: bytes, offset: int) -> tuple[int, int]:
    """Decode a BPS variable-length integer, returning (value, new_offset)."""
    value = 0
    shift = 1
    while True:
        b = data[offset]
        offset += 1
        value += (b & 0x7F) * shift
        if b & 0x80:
            break
        shift <<= 7
        value += shift
    return value, offset


def create_bps(source_bytes: bytes, target_bytes: bytes, metadata: str = "") -> bytes:
    """Create a BPS patch from source bytes to target bytes."""
    patch = bytearray(b"BPS1")
    patch.extend(encode_vli(len(source_bytes)))
    patch.extend(encode_vli(len(target_bytes)))

    meta_bytes = metadata.encode("utf-8")
    patch.extend(encode_vli(len(meta_bytes)))
    if meta_bytes:
        patch.extend(meta_bytes)

    # Encode linear differences
    source_len = len(source_bytes)
    target_len = len(target_bytes)
    target_offset = 0

    while target_offset < target_len:
        # Find run of matching bytes
        match_len = 0
        while (
            target_offset + match_len < target_len
            and target_offset + match_len < source_len
            and source_bytes[target_offset + match_len] == target_bytes[target_offset + match_len]
        ):
            match_len += 1

        if match_len > 0:
            # SourceRead command: action = 0
            # length = match_len
            # data = (length - 1) << 2 | 0
            patch.extend(encode_vli(((match_len - 1) << 2) | 0))
            target_offset += match_len
            continue

        # Find run of differing bytes
        diff_len = 0
        while target_offset + diff_len < target_len:
            if (
                target_offset + diff_len < source_len
                and source_bytes[target_offset + diff_len] == target_bytes[target_offset + diff_len]
            ):
                # Look ahead: if at least 4 bytes match, end diff run
                ahead = 0
                while (
                    target_offset + diff_len + ahead < target_len
                    and target_offset + diff_len + ahead < source_len
                    and source_bytes[target_offset + diff_len + ahead]
                    == target_bytes[target_offset + diff_len + ahead]
                ):
                    ahead += 1
                if ahead >= 4:
                    break
            diff_len += 1

        if diff_len > 0:
            # TargetRead command: action = 1
            patch.extend(encode_vli(((diff_len - 1) << 2) | 1))
            patch.extend(target_bytes[target_offset : target_offset + diff_len])
            target_offset += diff_len

    # Append checksums (source CRC, target CRC, patch CRC up to this point)
    source_crc = zlib.crc32(source_bytes) & 0xFFFFFFFF
    target_crc = zlib.crc32(target_bytes) & 0xFFFFFFFF
    patch.extend(struct.pack("<II", source_crc, target_crc))

    patch_crc = zlib.crc32(patch) & 0xFFFFFFFF
    patch.extend(struct.pack("<I", patch_crc))

    return bytes(patch)


def apply_bps(source_bytes: bytes, patch_bytes: bytes) -> bytes:
    """Apply a BPS patch to source bytes and verify CRCs."""
    assert patch_bytes[:4] == b"BPS1", "Invalid BPS header"

    offset = 4
    src_len, offset = decode_vli(patch_bytes, offset)
    dst_len, offset = decode_vli(patch_bytes, offset)
    meta_len, offset = decode_vli(patch_bytes, offset)
    offset += meta_len

    output = bytearray(dst_len)
    output_offset = 0
    source_relative_offset = 0
    target_relative_offset = 0

    footer_offset = len(patch_bytes) - 12
    while offset < footer_offset:
        data, offset = decode_vli(patch_bytes, offset)
        action = data & 3
        length = (data >> 2) + 1

        if action == 0:  # SourceRead
            output[output_offset : output_offset + length] = source_bytes[
                output_offset : output_offset + length
            ]
            output_offset += length
        elif action == 1:  # TargetRead
            output[output_offset : output_offset + length] = patch_bytes[
                offset : offset + length
            ]
            offset += length
            output_offset += length
        elif action == 2:  # SourceCopy
            shift, offset = decode_vli(patch_bytes, offset)
            source_relative_offset += -((shift >> 1) + 1) if (shift & 1) else (shift >> 1)
            output[output_offset : output_offset + length] = source_bytes[
                source_relative_offset : source_relative_offset + length
            ]
            output_offset += length
            source_relative_offset += length
        elif action == 3:  # TargetCopy
            shift, offset = decode_vli(patch_bytes, offset)
            target_relative_offset += -((shift >> 1) + 1) if (shift & 1) else (shift >> 1)
            for _ in range(length):
                output[output_offset] = output[target_relative_offset]
                output_offset += 1
                target_relative_offset += 1

    src_crc, dst_crc, p_crc = struct.unpack("<III", patch_bytes[footer_offset:])
    assert (
        zlib.crc32(source_bytes) & 0xFFFFFFFF == src_crc
    ), f"Source ROM CRC mismatch (expected {hex(src_crc)})"
    assert (
        zlib.crc32(output) & 0xFFFFFFFF == dst_crc
    ), f"Target ROM CRC mismatch (expected {hex(dst_crc)})"

    return bytes(output)


def main():
    if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
        os.chdir(os.environ["BUILD_WORKSPACE_DIRECTORY"])

    if len(sys.argv) < 4:
        print("Usage: create_bps.py <create|apply> <source.z64> <target.z64|patch.bps> [output]")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "create":
        source_path = Path(sys.argv[2])
        target_path = Path(sys.argv[3])
        out_path = Path(sys.argv[4]) if len(sys.argv) > 4 else target_path.with_suffix(".bps")

        patch = create_bps(source_path.read_bytes(), target_path.read_bytes())
        out_path.write_bytes(patch)
        print(f"Created patch: {out_path} ({len(patch)} bytes)")
    elif cmd == "apply":
        source_path = Path(sys.argv[2])
        patch_path = Path(sys.argv[3])
        out_path = Path(sys.argv[4]) if len(sys.argv) > 4 else source_path.with_name(source_path.stem + "-patched.z64")

        result = apply_bps(source_path.read_bytes(), patch_path.read_bytes())
        out_path.write_bytes(result)
        print(f"Applied patch to create: {out_path}")
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)


if __name__ == "__main__":
    main()
