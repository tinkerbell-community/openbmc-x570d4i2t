#!/usr/bin/env python3
"""
patch_rom.py — Download the ASRock X570D4I-2T 2.59C ROM and replace the
SendInfoBmcIpmiDxe module (GUID 9df02dfd-…) with a custom EFI binary.

Usage:
    python3 patch_rom.py <custom.efi> [--rom <rom_file>] [--out <output_rom>]

If --rom is omitted, the ROM is downloaded from ASRock's server and cached
to ~/.cache/x570d4i2t/X574I2T2.59C.

The custom EFI must be a valid PE32 image (no section header prepended).
It may be larger than the original 3104-byte SendInfoBmcIpmiDxe.efi.

Strategy:
  1. Decompress the LZMA-compressed inner FV (GUIDED section inside FFS 9E21FD93).
  2. Find the FFS file for SendInfoBmcIpmiDxe and replace it with a new FFS
     containing the custom PE32.  The new FFS reuses the original DEPEX, UI and
     VERSION sections; only the PE32 payload is swapped.
  3. Adjust the inner FV's free-space to absorb size changes (positive or negative).
  4. Recompress and patch the outer FFS and ROM bytes in place.
"""

import argparse, hashlib, io, os, struct, sys, urllib.request, uuid, zipfile
import lzma

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

ROM_URL      = "https://download.asrock.com/BIOS/Server/X570D4I-2T(2.59C)ROM.zip"
ROM_FILENAME = "X574I2T2.59C"          # filename inside the ZIP
ROM_CACHE    = os.path.expanduser("~/.cache/x570d4i2t")

# UEFI GUID bytes (little-endian struct pack of the standard text form)
# 9E21FD93-9C72-4C15-8C4B-E77F1DB2D792  — outer compressed FFS
OUTER_FFS_GUID = bytes([
    0x93,0xFD,0x21,0x9E, 0x72,0x9C, 0x15,0x4C,
    0x8C,0x4B, 0xE7,0x7F,0x1D,0xB2,0xD7,0x92,
])
# 9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24  — SendInfoBmcIpmiDxe (target)
TARGET_GUID = bytes([
    0xFD,0x2D,0xF0,0x9D, 0xF7,0x8C, 0xC7,0x4F,
    0xB8,0xAE, 0xCB,0xD9,0x56,0x0A,0x3F,0x24,
])

# Fresh GUID for our injected driver (4a2b7c8d-1234-5678-abcd-ef0123456789)
NEW_DXE_GUID = bytes([0x8d,0x7c,0x2b,0x4a, 0x34,0x12, 0x78,0x56,
    0xab,0xcd, 0xef,0x01,0x23,0x45,0x67,0x89])

# gEfiDxeAprioriFileNameGuid = FC510EE7-FFDC-11D4-BD41-0080C73C8881
APRIORI_DXE_GUID = bytes([0xe7,0x0e,0x51,0xfc, 0xdc,0xff, 0xd4,0x11,
    0xbd,0x41, 0x00,0x80,0xc7,0x3c,0x88,0x81])

# Main DXE FVMAIN_COMPACT.  The 32 MiB ROM contains TWO compressed DXE volumes,
# both with SendInfoBmcIpmiDxe + hundreds of drivers:
#   0x069F000 (8 MiB, lower half, 233 drivers)  — NOT dispatched (a second copy)
#   0x1AC1000 (4 MiB, upper half, 245 drivers)  — dispatched: it sits directly
#       below the boot block (0x1EA7000) whose top is 0x2000000 = the AMD reset
#       vector, so this is the FVMAIN the DXE core actually runs.
# Patching the lower copy leaves a valid-but-never-dispatched module (BIOS boots
# fine, our driver never runs).  Target the UPPER (dispatched) volume.
FV_OFFSET   = 0x01AC_1000
FV_SIZE     = 0x003E_6000   # 4 MiB FVMAIN_COMPACT (upper / dispatched)

# UEFI section types
SEC_GUID_DEFINED = 0x02
SEC_PE32         = 0x10
SEC_DXE_DEPEX    = 0x13
SEC_UI           = 0x15
SEC_VERSION      = 0x14
SEC_RAW          = 0x19

# EFI FFS types
FFS_TYPE_DRIVER  = 0x07

# ---------------------------------------------------------------------------
# Low-level UEFI helpers
# ---------------------------------------------------------------------------

def u24(buf, off):
    """Read a 3-byte little-endian integer."""
    return buf[off] | (buf[off+1] << 8) | (buf[off+2] << 16)

def pack24(n):
    return bytes([n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF])

def ffs_header_checksum(header_24: bytearray) -> bytearray:
    """
    Compute and write the FFS header checksum.

    Per the PI spec the Header checksum is computed with the State field (byte
    23) AND the IntegrityCheck.File field (byte 17) treated as ZERO, such that
    the 8-bit sum of the header is 0.  We must therefore exclude the real State
    byte (0xF8) from the sum — otherwise the checksum is off by 0xF8 and AMI
    afulnx rejects the image with "FFS Checksums ... Fail".
    """
    b = bytearray(header_24)
    b[16] = 0    # header checksum field itself
    b[17] = 0    # File checksum field (assumed 0)
    b[23] = 0    # State field (assumed 0)
    s = (-sum(b)) & 0xFF
    header_24[16] = s   # IntegrityCheck.Header
    # IntegrityCheck.File = 0xAA when FFS_ATTRIB_CHECKSUM is clear (it is)
    header_24[17] = 0xAA
    return header_24

def make_section(data: bytes, sec_type: int) -> bytes:
    """Build an EFI section (4-byte header + data), padded to 4-byte alignment."""
    size = len(data) + 4
    hdr = pack24(size) + bytes([sec_type])
    raw = hdr + data
    pad = (4 - len(raw) % 4) % 4
    return raw + b'\x00' * pad

def make_ffs(guid: bytes, sections: bytes, ffs_type: int = FFS_TYPE_DRIVER,
             attributes: int = 0x00) -> bytes:
    """
    Build a complete FFS file.

    The FFS file data (sections) is embedded after the 24-byte header.
    The returned bytes are padded to 8-byte alignment with 0xFF bytes
    (erased flash idiom).
    """
    size = 24 + len(sections)
    hdr = bytearray(24)
    hdr[:16] = guid
    # checksum bytes zeroed for now
    hdr[16] = 0
    hdr[17] = 0
    hdr[18] = ffs_type
    hdr[19] = attributes
    hdr[20:23] = pack24(size)
    hdr[23] = 0xF8   # EFI_FILE_DATA_VALID | EFI_FILE_HEADER_VALID
    ffs_header_checksum(hdr)

    raw = bytes(hdr) + sections
    pad = (8 - len(raw) % 8) % 8
    return raw + b'\xFF' * pad

def make_pad_file(total_size: int) -> bytes:
    """
    Build an EFI_FV_FILETYPE_FFS_PAD (0xF0) file of exactly total_size bytes
    (>=24).  The DXE core's FFS walk skips pad files and continues to the next
    real file — unlike a raw-0xFF gap, which it treats as end-of-FV free space.
    """
    hdr = bytearray(24)
    hdr[:16] = b'\x00' * 16        # pad-file name GUID = zeros (EDK2 convention;
                                   # avoids a 0xFF first byte being read as free)
    hdr[16] = 0; hdr[17] = 0
    hdr[18] = 0xF0                 # EFI_FV_FILETYPE_FFS_PAD
    hdr[19] = 0x00
    hdr[20:23] = pack24(total_size)
    hdr[23] = 0xF8                 # DATA_VALID | HEADER_VALID
    ffs_header_checksum(hdr)
    return bytes(hdr) + b'\xFF' * (total_size - 24)

# ---------------------------------------------------------------------------
# FV free-space handling
# ---------------------------------------------------------------------------

def find_free_start(fv: bytes, fv_hdr_len: int, fv_len: int) -> int:
    """Walk FFS files and return offset of the first free (0xFF) byte."""
    off = fv_hdr_len
    while off + 24 <= fv_len:
        if fv[off] == 0xFF:
            return off
        sz = u24(fv, off + 20)
        if sz == 0 or sz == 0xFFFFFF:
            return off
        off = (off + sz + 7) & ~7
    return off

def replace_ffs_in_fv(fv: bytearray, fv_hdr_len: int, fv_len: int,
                      old_offset: int, old_ffs_len: int,
                      new_ffs: bytes) -> bytearray:
    """
    Replace an FFS file in an FV.

    If new_ffs is smaller, the gap is filled with 0xFF (erased flash).
    If new_ffs is larger, the extra bytes are taken from the free space
    that follows.  Raises ValueError if there is not enough free space.
    """
    old_aligned = (old_ffs_len + 7) & ~7
    new_aligned = len(new_ffs)   # make_ffs already pads to 8

    free_start = find_free_start(fv, fv_hdr_len, fv_len)
    free_bytes  = fv_len - free_start

    delta = new_aligned - old_aligned
    if delta > free_bytes:
        raise ValueError(
            f"Not enough FV free space: need {delta} extra bytes, "
            f"have {free_bytes}")

    # Build new FV data
    before  = fv[:old_offset]
    after   = fv[old_offset + old_aligned:]
    if delta <= 0:
        # Smaller or same: fill the freed bytes with a PAD FILE (FFS type 0xF0),
        # NOT raw 0xFF.  A raw-0xFF gap looks like free space to the DXE core's
        # sequential FFS walk, which truncates the FV (later modules vanish and
        # a strict core may reject the whole FV).  A pad file keeps the chain.
        gapsize = -delta
        if gapsize == 0:
            gap = b''
        elif gapsize >= 24:
            gap = make_pad_file(gapsize)
        else:
            gap = b'\xFF' * gapsize   # too small for a header; harmless tail
        return bytearray(before + new_ffs + gap + after)
    else:
        # Larger: shift everything after our file DOWN by delta and absorb the
        # delta from the FV's TRAILING free space (drop delta bytes of 0xFF at
        # the very end).  (The old code trimmed delta bytes off the *front* of
        # the next file, corrupting it and truncating the FV.)
        combined = before + new_ffs + after          # = fv_len + delta long
        dropped = combined[fv_len:]
        if dropped.strip(b'\xFF'):
            raise ValueError("Not enough trailing free space to grow FFS")
        return bytearray(combined[:fv_len])

# ---------------------------------------------------------------------------
# FV header checksum
# ---------------------------------------------------------------------------

def update_fv_checksum(fv: bytearray) -> None:
    """Recalculate and write the FV header 16-bit checksum (EFI_FVH_CHECKSUM)."""
    hdr_len = struct.unpack_from('<H', fv, 0x30)[0]
    # Zero out the checksum field before summing
    fv[0x32] = 0
    fv[0x33] = 0
    total = 0
    for i in range(0, hdr_len, 2):
        total += struct.unpack_from('<H', fv, i)[0]
    checksum = (0x10000 - (total & 0xFFFF)) & 0xFFFF
    struct.pack_into('<H', fv, 0x32, checksum)

# ---------------------------------------------------------------------------
# EDK2-compatible LZMA compress / decompress
# ---------------------------------------------------------------------------

def lzma_decompress(data: bytes) -> bytes:
    return lzma.decompress(data, format=lzma.FORMAT_ALONE)

def lzma_compress(data: bytes) -> bytes:
    """
    Compress with LZMA1 in the EDK2 ALONE (raw) format.
    Properties: lc=3, lp=0, pb=2 → prop byte = (pb*5+lp)*9+lc = (10)*9+3 = 93 = 0x5d.
    Dictionary size 16 MiB (0x01000000) — matches the original ROM's dict_size so the
    AMI DXE core decompressor receives the same stream header it was tuned for.
    """
    filters = [{"id": lzma.FILTER_LZMA1, "preset": 6,
                "lc": 3, "lp": 0, "pb": 2, "dict_size": 1 << 24}]
    return lzma.compress(data, format=lzma.FORMAT_ALONE, filters=filters)

# ---------------------------------------------------------------------------
# Parse FV and locate a target FFS
# ---------------------------------------------------------------------------

def parse_fv(fv: bytes) -> tuple:
    """Return (hdr_len, fv_len) from FV header."""
    assert fv[0x28:0x2C] == b'_FVH', "Not an FV"
    fv_len  = struct.unpack_from('<Q', fv, 0x20)[0]
    hdr_len = struct.unpack_from('<H', fv, 0x30)[0]
    return hdr_len, fv_len

def ffs_content_start(fv: bytes) -> int:
    """
    Return the byte offset within fv where FFS file content begins.
    Accounts for the optional EFI_FIRMWARE_VOLUME_EXT_HEADER: if ExtHeaderOffset
    is non-zero, FFS content starts at ExtHeaderOffset + ExtHeaderSize (8-aligned).
    """
    hdr_len, _ = parse_fv(fv)
    ext_hdr_off = struct.unpack_from('<H', fv, 0x34)[0]
    if ext_hdr_off == 0:
        return hdr_len
    # EFI_FIRMWARE_VOLUME_EXT_HEADER: 16-byte FvName GUID + 4-byte ExtHeaderSize
    ext_hdr_size = struct.unpack_from('<I', fv, ext_hdr_off + 16)[0]
    return (ext_hdr_off + ext_hdr_size + 7) & ~7

def find_ffs_in_fv(fv: bytes, guid: bytes) -> tuple:
    """
    Scan an FV for an FFS file with matching GUID.
    Uses direct GUID search rather than sequential FFS walking so it works
    even when non-FFS AMI structures occupy space between files.
    Returns (offset, ffs_size) or (None, None).
    """
    _, fv_len = parse_fv(fv)
    idx = 0
    while True:
        idx = fv.find(guid, idx)
        if idx == -1 or idx + 24 > fv_len:
            return None, None
        # FFS files are 8-byte aligned in the FV
        if idx % 8 != 0:
            idx += 1
            continue
        sz = u24(fv, idx + 20)
        state = fv[idx + 23]
        # Valid state for an accessible file (erase polarity 1 → set bits = 0)
        if sz >= 24 and sz <= fv_len and state in (0xF8, 0xFC, 0xFE, 0x07):
            return idx, sz
        idx += 1

def extract_sections(ffs: bytes) -> list:
    """
    Return list of (sec_type, sec_data) tuples for all sections in an FFS file.
    """
    off = 24   # FFS header
    ffs_size = u24(ffs, 20)
    sections = []
    while off + 4 <= ffs_size:
        sec_sz   = u24(ffs, off)
        sec_type = ffs[off + 3]
        data     = ffs[off + 4 : off + sec_sz]
        sections.append((sec_type, data))
        off += (sec_sz + 3) & ~3
    return sections

# ---------------------------------------------------------------------------
# Main patch logic
# ---------------------------------------------------------------------------

def get_rom(rom_path: str) -> bytes:
    if rom_path:
        print(f"Reading ROM from {rom_path}")
        return open(rom_path, 'rb').read()

    cached = os.path.join(ROM_CACHE, ROM_FILENAME)
    if os.path.exists(cached):
        print(f"Using cached ROM: {cached}")
        return open(cached, 'rb').read()

    print(f"Downloading ROM from {ROM_URL} ...")
    os.makedirs(ROM_CACHE, exist_ok=True)
    zip_path = cached + ".zip"
    urllib.request.urlretrieve(ROM_URL, zip_path)
    print(f"Extracting {ROM_FILENAME} ...")
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        match = [n for n in names if ROM_FILENAME in n]
        if not match:
            raise FileNotFoundError(
                f"{ROM_FILENAME} not found in ZIP; contents: {names}")
        with z.open(match[0]) as src, open(cached, 'wb') as dst:
            dst.write(src.read())
    print(f"ROM cached to {cached}")
    return open(cached, 'rb').read()

def patch_one_fv(rom: bytearray, fv_offset: int, fv_size: int, custom_efi: bytes,
                  label: str) -> None:
    """Patch one outer FV at fv_offset/fv_size in-place in rom."""
    fv = rom[fv_offset : fv_offset + fv_size]
    fv_hdr_len, fv_len = parse_fv(fv)
    print(f"[{label}] Outer FV: hdr_len=0x{fv_hdr_len:x} fv_len=0x{fv_len:x}")

    outer_ffs_off, outer_ffs_sz = find_ffs_in_fv(fv, OUTER_FFS_GUID)
    if outer_ffs_off is None:
        print(f"[{label}] WARNING: LZMA container not found, skipping")
        return
    print(f"[{label}] LZMA container at FV+0x{outer_ffs_off:x}, size=0x{outer_ffs_sz:x}")

    outer_ffs = fv[outer_ffs_off : outer_ffs_off + outer_ffs_sz]
    sec_off_in_ffs = 24
    guided_data_offset = guided_sec_start = guided_sec_size = None
    while sec_off_in_ffs + 4 <= outer_ffs_sz:
        sec_sz   = u24(outer_ffs, sec_off_in_ffs)
        sec_type = outer_ffs[sec_off_in_ffs + 3]
        if sec_type == SEC_GUID_DEFINED:
            guided_data_offset = struct.unpack_from('<H', outer_ffs, sec_off_in_ffs + 20)[0]
            guided_sec_start   = sec_off_in_ffs
            guided_sec_size    = sec_sz
            break
        sec_off_in_ffs += (sec_sz + 3) & ~3
    if guided_data_offset is None:
        print(f"[{label}] WARNING: GUID-defined section not found, skipping")
        return

    compressed_start = guided_sec_start + guided_data_offset
    compressed_data  = outer_ffs[compressed_start : guided_sec_start + guided_sec_size]
    print(f"[{label}] LZMA payload: {len(compressed_data)} bytes")

    print(f"[{label}] Decompressing inner FV ...")
    inner_raw = lzma_decompress(compressed_data)
    print(f"[{label}] Decompressed: {len(inner_raw)} bytes")

    INNER_FV_SECTION_OFF = 0x0C
    INNER_FV_OFF         = 0x10
    inner_fv = bytearray(inner_raw[INNER_FV_OFF:])
    inner_fv_hdr_len, inner_fv_len = parse_fv(inner_fv)
    print(f"[{label}] Inner FV: hdr_len=0x{inner_fv_hdr_len:x} fv_len=0x{inner_fv_len:x}")

    target_off, target_sz = find_ffs_in_fv(inner_fv, TARGET_GUID)
    if target_off is None:
        print(f"[{label}] WARNING: SendInfoBmcIpmiDxe not found, skipping")
        return
    print(f"[{label}] Target FFS at inner FV+0x{target_off:x}, size=0x{target_sz:x}")

    old_ffs_bytes = bytes(inner_fv[target_off : target_off + target_sz])
    old_sections  = extract_sections(old_ffs_bytes)
    keep_sections = b''
    TRUE_DEPEX = b'\x06\x08'
    keep_sections += make_section(TRUE_DEPEX, SEC_DXE_DEPEX)
    for (st, sd) in old_sections:
        if st in (SEC_UI, SEC_VERSION):
            keep_sections += make_section(sd, st)
    keep_sections += make_section(custom_efi, SEC_PE32)

    new_ffs = make_ffs(TARGET_GUID, keep_sections, FFS_TYPE_DRIVER)
    print(f"[{label}] New FFS: {len(new_ffs)} bytes  (original: {(target_sz+7)&~7} bytes)")

    inner_fv = replace_ffs_in_fv(inner_fv, inner_fv_hdr_len, inner_fv_len,
                                   target_off, target_sz, new_ffs)
    update_fv_checksum(inner_fv)

    new_inner_raw = bytearray(inner_raw[:INNER_FV_OFF]) + inner_fv
    new_fv_sec_size = 4 + len(inner_fv)
    new_inner_raw[INNER_FV_SECTION_OFF:INNER_FV_SECTION_OFF+3] = pack24(new_fv_sec_size)

    print(f"[{label}] Recompressing inner FV ...")
    new_compressed = lzma_compress(bytes(new_inner_raw))
    print(f"[{label}] Recompressed: {len(new_compressed)} bytes")

    old_guided_hdr = outer_ffs[guided_sec_start : guided_sec_start + guided_data_offset]
    new_guided_sec_size = guided_data_offset + len(new_compressed)
    new_guided_hdr = bytearray(old_guided_hdr)
    new_guided_hdr[0:3] = pack24(new_guided_sec_size)
    new_guided_section = bytes(new_guided_hdr) + new_compressed

    new_outer_sections = outer_ffs[24 : 24 + guided_sec_start - 24] + new_guided_section
    new_outer_ffs = make_ffs(OUTER_FFS_GUID, new_outer_sections, ffs_type=0x0B)

    outer_fv = bytearray(fv)
    outer_ffs_aligned = (outer_ffs_sz + 7) & ~7
    outer_fv = replace_ffs_in_fv(outer_fv, fv_hdr_len, fv_len,
                                   outer_ffs_off, outer_ffs_sz, new_outer_ffs)
    update_fv_checksum(outer_fv)
    print(f"[{label}] Outer FV patched")

    rom[fv_offset : fv_offset + fv_size] = outer_fv


# Both outer FVs in the ROM — the upper (0x01AC1000) and lower (0x069F000).
# Each contains an LZMA-compressed inner FV with the real DXE drivers.
# We append our driver as an UNCOMPRESSED FFS directly into each outer FV's
# trailing free space instead of replacing a compressed module.  This bypasses
# LZMA recompression entirely (which was the root cause of v6-v11 failures:
# the original uses lc=3 lp=0 pb=2 dict_size=16MB but Python's lzma recompresses
# with dict_size=1MB, and the AMI DXE core decompressor rejects the mismatch).
LOWER_FV_OFFSET = 0x069F_000
LOWER_FV_SIZE   = 0x0080_8000   # 8 MiB


def find_last_ffs_end(fv: bytes, fv_len: int) -> int:
    """
    Return byte offset immediately after the last valid FFS file in the FV.
    Unlike find_free_start(), this reads size+state fields rather than checking
    fv[off]==0xFF, so it correctly handles AMI PAD files whose GUID is all-0xFF.
    """
    ext_off = struct.unpack_from('<H', fv, 0x34)[0]
    if ext_off:
        ext_sz = struct.unpack_from('<I', fv, ext_off + 16)[0]
        off = (ext_off + ext_sz + 7) & ~7
    else:
        off = struct.unpack_from('<H', fv, 0x30)[0]  # hdr_len

    last_end = off
    while off + 24 <= fv_len:
        sz    = u24(fv, off + 20)
        state = fv[off + 23]
        if sz >= 24 and sz <= fv_len - off and state in (0xF8, 0xFC, 0xFE, 0x07):
            last_end = (off + sz + 7) & ~7
            off = last_end
        else:
            break
    return last_end


def append_dxe_to_fv(rom: bytearray, fv_offset: int, fv_size: int,
                     new_ffs: bytes, label: str) -> None:
    """Append an FFS file into the trailing free space of an outer FV."""
    fv = bytearray(rom[fv_offset : fv_offset + fv_size])
    _, fv_len = parse_fv(fv)

    last_end = find_last_ffs_end(bytes(fv), fv_len)
    free = fv_len - last_end
    print(f"[{label}] Last FFS ends at FV+0x{last_end:x}, free=0x{free:x} ({free//1024} KB)")
    if free < len(new_ffs):
        raise ValueError(f"[{label}] Insufficient free space: {free} < {len(new_ffs)}")

    fv[last_end : last_end + len(new_ffs)] = new_ffs
    update_fv_checksum(fv)
    rom[fv_offset : fv_offset + fv_size] = fv
    print(f"[{label}] DXE FFS appended (GUID {uuid.UUID(bytes_le=NEW_DXE_GUID)})")


def insert_ffs_into_inner_fv(rom: bytearray, fv_offset: int, fv_size: int,
                              new_ffs: bytes, label: str) -> None:
    """
    Decompress the LZMA container in an outer FV, replace TARGET_GUID
    (SendInfoBmcIpmiDxe) with new_ffs inside the inner FV, recompress,
    and splice back.  The size delta is absorbed by the inner FV's trailing
    free space; the outer FV free space absorbs any recompression growth.
    """
    fv = rom[fv_offset : fv_offset + fv_size]
    fv_hdr_len, fv_len = parse_fv(fv)
    print(f"[{label}] Outer FV: hdr_len=0x{fv_hdr_len:x} fv_len=0x{fv_len:x}")

    outer_ffs_off, outer_ffs_sz = find_ffs_in_fv(fv, OUTER_FFS_GUID)
    if outer_ffs_off is None:
        print(f"[{label}] WARNING: LZMA container not found, skipping")
        return
    print(f"[{label}] LZMA container at FV+0x{outer_ffs_off:x} size=0x{outer_ffs_sz:x}")

    outer_ffs = fv[outer_ffs_off : outer_ffs_off + outer_ffs_sz]
    sec_off = 24
    guided_data_offset = guided_sec_start = guided_sec_size = None
    while sec_off + 4 <= outer_ffs_sz:
        ss = u24(outer_ffs, sec_off)
        st = outer_ffs[sec_off + 3]
        if st == SEC_GUID_DEFINED:
            guided_data_offset = struct.unpack_from('<H', outer_ffs, sec_off + 20)[0]
            guided_sec_start   = sec_off
            guided_sec_size    = ss
            break
        sec_off += (ss + 3) & ~3
    if guided_data_offset is None:
        print(f"[{label}] WARNING: GUID-defined section not found, skipping")
        return

    compressed_start = guided_sec_start + guided_data_offset
    compressed_data  = outer_ffs[compressed_start : guided_sec_start + guided_sec_size]

    print(f"[{label}] Decompressing ...")
    inner_raw = lzma_decompress(compressed_data)
    print(f"[{label}] Decompressed: {len(inner_raw)} bytes")

    INNER_FV_OFF = 0x10
    inner_fv = bytearray(inner_raw[INNER_FV_OFF:])
    inner_fv_hdr_len, inner_fv_len = parse_fv(inner_fv)
    print(f"[{label}] Inner FV: hdr_len=0x{inner_fv_hdr_len:x} fv_len=0x{inner_fv_len:x}")

    target_off, target_sz = find_ffs_in_fv(bytes(inner_fv), TARGET_GUID)
    if target_off is None:
        raise ValueError(f"[{label}] TARGET_GUID (SendInfoBmcIpmiDxe) not found in inner FV")
    old_aligned = (target_sz + 7) & ~7
    delta = len(new_ffs) - old_aligned
    free_end = find_last_ffs_end(bytes(inner_fv), inner_fv_len)
    free_bytes = inner_fv_len - free_end
    print(f"[{label}] Replacing TARGET_GUID at iFV+0x{target_off:x} size=0x{target_sz:x} -> 0x{len(new_ffs):x} (delta={delta:+d}), free={free_bytes}B")
    if delta > free_bytes:
        raise ValueError(f"[{label}] Not enough inner FV free space: need {delta}, have {free_bytes}")
    inner_fv = replace_ffs_in_fv(inner_fv, inner_fv_hdr_len, inner_fv_len,
                                  target_off, target_sz, new_ffs)
    update_fv_checksum(inner_fv)
    print(f"[{label}] Replaced TARGET_GUID with new DXE FFS")

    # Replace the last APRIORI entry with our GUID so AMI DxeCore (which appears
    # to have a 15-entry max for APRIORI) dispatches us without growing the list.
    ap_off, ap_sz = find_ffs_in_fv(bytes(inner_fv), APRIORI_DXE_GUID)
    if ap_off is not None:
        sec_off2 = ap_off + 24
        while sec_off2 + 4 <= ap_off + ap_sz:
            raw_ss = u24(bytes(inner_fv), sec_off2)
            raw_st = inner_fv[sec_off2 + 3]
            if raw_st == SEC_RAW:
                old_guids = bytearray(inner_fv[sec_off2+4 : sec_off2+raw_ss])
                n = len(old_guids) // 16
                # Replace the last entry with our GUID (same list length, zero delta)
                old_guids[(n-1)*16 : n*16] = NEW_DXE_GUID
                new_raw_sec = pack24(4 + len(old_guids)) + bytes([SEC_RAW]) + bytes(old_guids)
                pad = (4 - len(new_raw_sec) % 4) % 4
                new_raw_sec += b'\x00' * pad
                new_ap_ffs = make_ffs(APRIORI_DXE_GUID, new_raw_sec, ffs_type=0x0B)
                ap_delta = len(new_ap_ffs) - ((ap_sz + 7) & ~7)
                print(f"[{label}] APRIORI: replaced entry #{n-1} with our GUID (delta={ap_delta:+d})")
                inner_fv = replace_ffs_in_fv(inner_fv, inner_fv_hdr_len, inner_fv_len,
                                              ap_off, ap_sz, new_ap_ffs)
                update_fv_checksum(inner_fv)
                break
            sec_off2 += (raw_ss + 3) & ~3
    else:
        print(f"[{label}] WARNING: no APRIORI file found")

    new_inner_raw = bytearray(inner_raw[:INNER_FV_OFF]) + inner_fv
    new_fv_sec_size = 4 + len(inner_fv)
    new_inner_raw[0x0C:0x0F] = pack24(new_fv_sec_size)

    print(f"[{label}] Recompressing ...")
    new_compressed = lzma_compress(bytes(new_inner_raw))
    print(f"[{label}] Recompressed: {len(new_compressed)} bytes (orig {len(compressed_data)})")

    old_guided_hdr = outer_ffs[guided_sec_start : guided_sec_start + guided_data_offset]
    new_guided_sec_size = guided_data_offset + len(new_compressed)
    new_guided_hdr = bytearray(old_guided_hdr)
    new_guided_hdr[0:3] = pack24(new_guided_sec_size)
    new_guided_section = bytes(new_guided_hdr) + new_compressed

    new_outer_sections = outer_ffs[24 : 24 + guided_sec_start - 24] + new_guided_section
    new_outer_ffs = make_ffs(OUTER_FFS_GUID, new_outer_sections, ffs_type=0x0B)

    outer_fv = bytearray(fv)
    outer_fv = replace_ffs_in_fv(outer_fv, fv_hdr_len, fv_len,
                                  outer_ffs_off, outer_ffs_sz, new_outer_ffs)
    update_fv_checksum(outer_fv)
    rom[fv_offset : fv_offset + fv_size] = outer_fv
    print(f"[{label}] Done")


def patch_rom(custom_efi: bytes, rom: bytes) -> bytes:
    rom = bytearray(rom)

    # Build a DXE driver FFS with TRUE DEPEX and a fresh GUID.
    # Inserted into the compressed inner FV's free space so SendInfoBmcIpmiDxe
    # stays intact — avoiding any GUID-specific PE32 integrity check on the
    # replaced module.  dict_size=16MB matches the original so the AMI LZMA
    # decompressor accepts the recompressed stream.
    TRUE_DEPEX = b'\x06\x08'
    sections = (make_section(TRUE_DEPEX, SEC_DXE_DEPEX) +
                make_section(custom_efi, SEC_PE32))
    new_ffs = make_ffs(NEW_DXE_GUID, sections, FFS_TYPE_DRIVER)
    print(f"DXE FFS: {len(new_ffs)} bytes  GUID={uuid.UUID(bytes_le=NEW_DXE_GUID)}")

    insert_ffs_into_inner_fv(rom, FV_OFFSET,       FV_SIZE,       new_ffs, "upper")
    insert_ffs_into_inner_fv(rom, LOWER_FV_OFFSET, LOWER_FV_SIZE, new_ffs, "lower")

    print("ROM patched successfully")
    return bytes(rom)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('custom_efi', help='Path to custom PE32 EFI binary')
    ap.add_argument('--rom', default=None,
                    help='ROM file to patch (default: download from ASRock)')
    ap.add_argument('--out', default=None,
                    help='Output ROM path (default: <input>.patched)')
    args = ap.parse_args()

    custom_efi = open(args.custom_efi, 'rb').read()
    if custom_efi[:2] != b'MZ':
        sys.exit(f"ERROR: {args.custom_efi} does not look like a PE32 (no MZ header)")
    print(f"Custom EFI: {args.custom_efi} ({len(custom_efi)} bytes)")

    rom = get_rom(args.rom)
    print(f"ROM: {len(rom)} bytes")

    patched = patch_rom(custom_efi, rom)

    out_path = args.out or ((args.rom or os.path.join(ROM_CACHE, ROM_FILENAME)) + '.patched')
    open(out_path, 'wb').write(patched)

    sha = hashlib.sha256(patched).hexdigest()[:16]
    print(f"\nWrote {len(patched)} bytes → {out_path}")
    print(f"SHA256 prefix: {sha}")
    print("\nTo flash (from a Linux host with flashrom + BMC IPMI access):")
    print(f"  ipmitool -I lanplus -H 10.0.80.1 -U root -P '0penBmc' chassis power off")
    print(f"  flashrom -p linux_spi:dev=/dev/spidev0.0 -w {out_path}")


if __name__ == '__main__':
    main()
