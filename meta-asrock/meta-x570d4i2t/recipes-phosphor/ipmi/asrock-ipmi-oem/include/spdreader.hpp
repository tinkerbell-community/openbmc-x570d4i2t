// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// DDR4 DIMM SPD reader for the X570D4I-2T.
//
// The four DDR4 SPD EEPROMs sit on i2c bus 7 (AST2500 1e78a300.i2c) at
// addresses 0x50..0x53, with the JEDEC EE1004 page-select pseudo-addresses
// SPA0=0x36 (page 0) / SPA1=0x37 (page 1). Reading is done with raw /dev/i2c-7
// transactions (no ee1004 kernel driver required): the technical fields (size,
// speed, ranks, ECC) live on page 0 (bytes 0..255); the manufacturer / serial /
// part-number strings live on page 1 (bytes 256..511, read at offsets 0x40..).
//
// SPD content is static, so callers should read once and cache. The page-select
// state is global to the bus, so every page-1 read is bracketed by a select /
// reset and a byte-2 (== 0x0C DDR4) sanity check + retry.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spd
{

inline constexpr int      kSpdBus       = 7;     // i2c-7
inline constexpr uint8_t  kSpdBaseAddr  = 0x50;  // first DIMM SPD
inline constexpr int      kSpdSlots     = 4;     // 0x50..0x53
inline constexpr uint8_t  kSpdMemTypeDDR4 = 0x0C; // SPD byte 2
inline constexpr uint8_t  kSmbiosTypeDDR4 = 0x1A; // SMBIOS Type 17 MemoryType

struct DimmInfo
{
    bool        present     = false;
    uint8_t     slotIndex   = 0;      // 0..3 (SPD addr = kSpdBaseAddr + slotIndex)
    uint8_t     spdAddr      = 0;      // 0x50..0x53
    uint32_t    sizeMiB      = 0;      // module capacity in MiB
    uint16_t    speedMTs     = 0;      // configured/standard data rate, MT/s
    uint8_t     ranks        = 0;      // number of package ranks
    uint8_t     deviceWidth  = 0;      // SDRAM device width in bits (4/8/16/32)
    uint8_t     busWidthBits = 0;      // primary bus width (e.g. 64)
    bool        ecc          = false;  // true if 8-bit ECC extension present
    uint8_t     smbiosMemType = kSmbiosTypeDDR4;
    std::string manufacturer;          // decoded JEDEC vendor (or "JEDEC xx-xx")
    std::string partNumber;            // trimmed ASCII
    std::string serial;                // hex, e.g. "010461DD"
    uint16_t    jedecMfrId   = 0;      // (bank<<8)|code, raw bytes 320/321
};

// Read SPD for every populated DDR4 DIMM on `bus` at base..base+count-1.
// Returns one entry per address; .present=false for empty/non-DDR4 slots.
std::vector<DimmInfo> readAllDimms(int bus = kSpdBus, uint8_t base = kSpdBaseAddr,
                                   int count = kSpdSlots);

// Decode a JEDEC manufacturer (continuation byte, code byte) to a vendor name.
std::string decodeJedecManufacturer(uint8_t bankByte, uint8_t codeByte);

} // namespace spd
