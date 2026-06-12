// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// spd:: DDR4 SPD reader implementation — raw /dev/i2c-<bus> EE1004 access.

#include <spdreader.hpp>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <phosphor-logging/log.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace spd
{

namespace
{

// EE1004 page-select pseudo-addresses (SPA0/SPA1).
constexpr uint8_t kSpaPage0 = 0x36;
constexpr uint8_t kSpaPage1 = 0x37;

// SPD byte offsets (page 0 = 0..255).
constexpr uint8_t kP0MemType      = 0x02; // 0x0C = DDR4
constexpr uint8_t kP0Density      = 0x04; // SDRAM density / banks
constexpr uint8_t kP0ModuleOrg    = 0x0C; // device width + ranks
constexpr uint8_t kP0BusWidth     = 0x0D; // primary bus width + ECC ext
constexpr uint8_t kP0FineTckMin   = 0x7D; // byte 125 tCKAVGmin fine offset (FTB, signed)
constexpr uint8_t kP0MtbTckMin    = 0x12; // tCKAVGmin (MTB units, 125 ps)

// Page 1 (bytes 256..511) read at device offsets 0x40.. after selecting SPA1.
constexpr uint8_t kP1MfrBankOff   = 0x40; // byte 320 (continuation count)
constexpr uint8_t kP1MfrCodeOff   = 0x41; // byte 321 (manufacturer code)
constexpr uint8_t kP1SerialOff    = 0x45; // bytes 325..328 (4-byte serial)
constexpr uint8_t kP1PartOff      = 0x49; // bytes 329..348 (20-char part no.)
constexpr uint8_t kPartLen        = 20;

int setSlave(int fd, uint8_t addr)
{
    return ::ioctl(fd, I2C_SLAVE_FORCE, addr);
}

// Select EE1004 page (0 or 1). Best-effort: the SPA address may NAK the data
// phase on some modules yet still latch the page, so a short write is tolerated.
void selectPage(int fd, int page)
{
    uint8_t spa = (page == 1) ? kSpaPage1 : kSpaPage0;
    if (setSlave(fd, spa) < 0)
    {
        return;
    }
    uint8_t zero = 0x00;
    (void)::write(fd, &zero, 1);
}

// Random-read `len` bytes at `off` from device `devAddr` (write offset, read).
bool readAt(int fd, uint8_t devAddr, uint8_t off, uint8_t* buf, size_t len)
{
    if (setSlave(fd, devAddr) < 0)
    {
        return false;
    }
    if (::write(fd, &off, 1) != 1)
    {
        return false;
    }
    return ::read(fd, buf, len) == static_cast<ssize_t>(len);
}

uint32_t ddr4ModuleSizeMiB(uint8_t densityByte, uint8_t orgByte, uint8_t busByte)
{
    // JEDEC DDR4 SPD (Annex L). Per-die capacity in Mbit:
    static constexpr std::array<uint32_t, 16> kDieMbit = {
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
        12288, 24576, 3072, 6144, 18432, 0, 0, 0};
    uint32_t dieMbit  = kDieMbit[densityByte & 0x0F];
    uint8_t  widthSel = orgByte & 0x07;          // SDRAM device width
    uint32_t sdramWidth = (widthSel <= 3) ? (4u << widthSel) : 0; // 4/8/16/32
    uint8_t  busSel   = busByte & 0x07;          // primary bus width
    uint32_t busWidth = (busSel <= 3) ? (8u << busSel) : 0;       // 8/16/32/64
    uint8_t  ranks    = ((orgByte >> 3) & 0x07) + 1;
    if (dieMbit == 0 || sdramWidth == 0 || busWidth == 0)
    {
        return 0;
    }
    // capacity(MB) = dieMbit/8 * busWidth/sdramWidth * ranks
    return (dieMbit / 8) * (busWidth / sdramWidth) * ranks;
}

uint16_t ddr4SpeedMTs(uint8_t mtbTck, int8_t ftbTck)
{
    // tCK(ns) = mtb*0.125 + ftb*0.001 ; data rate = round(2/tCK) in MT/s,
    // snapped to the nearest standard JEDEC speed bin.
    double tckNs = mtbTck * 0.125 + ftbTck * 0.001;
    if (tckNs <= 0.0)
    {
        return 0;
    }
    double mts = 2000.0 / tckNs;
    static constexpr std::array<uint16_t, 7> kBins = {
        1600, 1866, 2133, 2400, 2666, 2933, 3200};
    uint16_t best = 0;
    double   bestErr = 1e9;
    for (uint16_t b : kBins)
    {
        double e = (mts > b) ? (mts - b) : (b - mts);
        if (e < bestErr)
        {
            bestErr = e;
            best = b;
        }
    }
    return best;
}

} // namespace

std::string decodeJedecManufacturer(uint8_t bankByte, uint8_t codeByte)
{
    uint8_t code = codeByte & 0x7F; // strip odd-parity bit
    uint8_t bank = bankByte & 0x7F; // number of 0x7F continuation codes
    // Small map of vendors seen on this platform; extend as needed.
    // Key = (bank, 7-bit code).
    struct Entry { uint8_t bank; uint8_t code; const char* name; };
    static constexpr std::array<Entry, 9> kVendors = {{
        {0, 0x2C, "Micron"},
        {0, 0x4F, "Transcend"},
        {1, 0x4D, "Samsung"},   // bank 1 example
        {5, 0x6F, "Team Group"},// TeamGroup (bankByte 0x04 -> bank 5)
        {1, 0x98, "Kingston"},
        {3, 0x0B, "Crucial/Nanya"},
        {1, 0x9E, "Corsair"},
        {1, 0x94, "G.Skill"},
        {0, 0xAD, "SK Hynix"},
    }};
    // bankByte stores (continuation_count); JEP106 bank index = count + 1.
    uint8_t bankIdx = static_cast<uint8_t>(bank + 1);
    for (const auto& v : kVendors)
    {
        if (v.bank == bankIdx && v.code == code)
        {
            return v.name;
        }
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "JEDEC %02X-%02X", bankByte, codeByte);
    return buf;
}

std::vector<DimmInfo> readAllDimms(int bus, uint8_t base, int count)
{
    std::vector<DimmInfo> out;
    out.reserve(static_cast<size_t>(count));

    char dev[32];
    std::snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
    int fd = ::open(dev, O_RDWR);
    if (fd < 0)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "spd::readAllDimms: cannot open i2c bus",
            phosphor::logging::entry("DEV=%s", dev));
        return out;
    }

    // i2c-7 is shared (FRU EEPROM + hwmon sensors) and the EE1004 page-select
    // (SPA0/SPA1) is GLOBAL bus state, so a concurrent transaction can corrupt
    // a multi-step SPD read. Retry each DIMM until the read is self-consistent
    // (byte 2 == DDR4 and the page-1 part number is printable).
    constexpr int kReadRetries = 6;

    for (int i = 0; i < count; ++i)
    {
        DimmInfo d;
        d.slotIndex = static_cast<uint8_t>(i);
        d.spdAddr   = static_cast<uint8_t>(base + i);

        bool decoded = false;
        for (int attempt = 0; attempt < kReadRetries && !decoded; ++attempt)
        {
            // --- page 0: presence + geometry ---
            selectPage(fd, 0);
            uint8_t memType = 0;
            if (!readAt(fd, d.spdAddr, kP0MemType, &memType, 1))
            {
                continue; // transient bus error -> retry
            }
            if (memType != kSpdMemTypeDDR4)
            {
                break;    // genuinely empty / not DDR4 -> leave present=false
            }

            uint8_t density = 0, org = 0, busw = 0, mtb = 0;
            int8_t  ftb = 0;
            readAt(fd, d.spdAddr, kP0Density, &density, 1);
            readAt(fd, d.spdAddr, kP0ModuleOrg, &org, 1);
            readAt(fd, d.spdAddr, kP0BusWidth, &busw, 1);
            readAt(fd, d.spdAddr, kP0MtbTckMin, &mtb, 1);
            readAt(fd, d.spdAddr, kP0FineTckMin,
                   reinterpret_cast<uint8_t*>(&ftb), 1);

            // --- page 1: manufacturer / serial / part ---
            selectPage(fd, 1);
            uint8_t mfr[2] = {0, 0};
            uint8_t ser[4] = {0};
            uint8_t part[kPartLen] = {0};
            bool gotMfr  = readAt(fd, d.spdAddr, kP1MfrBankOff, mfr, 2);
            bool gotSer  = readAt(fd, d.spdAddr, kP1SerialOff, ser, 4);
            bool gotPart = readAt(fd, d.spdAddr, kP1PartOff, part, kPartLen);
            selectPage(fd, 0); // leave the bus on page 0

            // Validate the page-1 read integrity: a real part number starts
            // with a printable char and contains no control bytes (a corrupted
            // page-select read yields garbage / wrong-page data).
            std::string p(reinterpret_cast<char*>(part), kPartLen);
            size_t end = p.find_last_not_of(std::string("\0 ", 2));
            std::string trimmed =
                (end == std::string::npos) ? "" : p.substr(0, end + 1);
            bool partOk = gotPart && !trimmed.empty() &&
                          static_cast<unsigned char>(trimmed[0]) >= 0x20;
            for (unsigned char c : trimmed)
            {
                if (c < 0x20 || c >= 0x7f)
                {
                    partOk = false;
                    break;
                }
            }
            if (!partOk)
            {
                continue; // corrupted page-1 read -> retry
            }

            d.present      = true;
            d.deviceWidth  = (org & 0x07) <= 3 ? (4u << (org & 0x07)) : 0;
            d.ranks        = static_cast<uint8_t>(((org >> 3) & 0x07) + 1);
            d.busWidthBits = (busw & 0x07) <= 3 ? (8u << (busw & 0x07)) : 0;
            d.ecc          = ((busw >> 3) & 0x07) != 0;
            d.sizeMiB      = ddr4ModuleSizeMiB(density, org, busw);
            d.speedMTs     = ddr4SpeedMTs(mtb, ftb);
            if (gotMfr)
            {
                d.jedecMfrId   = static_cast<uint16_t>((mfr[0] << 8) | mfr[1]);
                d.manufacturer = decodeJedecManufacturer(mfr[0], mfr[1]);
            }
            if (gotSer)
            {
                char sbuf[9];
                std::snprintf(sbuf, sizeof(sbuf), "%02X%02X%02X%02X",
                              ser[0], ser[1], ser[2], ser[3]);
                d.serial = sbuf;
            }
            d.partNumber = trimmed;
            decoded = true;

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "spd::readAllDimms: DIMM decoded",
                phosphor::logging::entry("ADDR=0x%02X", d.spdAddr),
                phosphor::logging::entry("ATTEMPT=%d", attempt + 1),
                phosphor::logging::entry("SIZE_MIB=%u", d.sizeMiB),
                phosphor::logging::entry("SPEED=%u", d.speedMTs),
                phosphor::logging::entry("PART=%s", d.partNumber.c_str()));
        }

        if (!decoded && d.spdAddr)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "spd::readAllDimms: slot empty or unreadable",
                phosphor::logging::entry("ADDR=0x%02X", d.spdAddr));
        }
        out.push_back(d);
    }

    ::close(fd);
    return out;
}

} // namespace spd
