// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// smbiosbuild:: SMBIOS table synthesizer implementation.

#include <smbiosbuilder.hpp>
#include <spdreader.hpp>

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace smbiosbuild
{

using phosphor::logging::entry;
using phosphor::logging::level;
using phosphor::logging::log;

// ---------------------------------------------------------------------------
// Captured host state (set by the 0xB2 / 0xB5 IPMI handlers).
// ---------------------------------------------------------------------------
namespace
{
std::string g_biosVersion;     // e.g. "2.59C"
std::string g_biosReleaseDate; // e.g. "11/14/2022"
std::string g_boardName;       // e.g. "X570D4I-2T"
} // namespace

void setHostBios(const std::string& version, const std::string& releaseDate)
{
    if (!version.empty())
    {
        g_biosVersion = version;
    }
    if (!releaseDate.empty())
    {
        g_biosReleaseDate = releaseDate;
    }
}

void setHostBoardName(const std::string& productName)
{
    if (!productName.empty())
    {
        g_boardName = productName;
    }
}

// ---------------------------------------------------------------------------
// SMBIOS structure writer: fixed formatted area + appended string set.
// ---------------------------------------------------------------------------
namespace
{

// Static board constants.
constexpr const char* kSysManufacturer   = "ASRockRack";
constexpr const char* kSysProductDefault  = "X570D4I-2T";
constexpr const char* kBiosVendor         = "American Megatrends International, LLC.";
constexpr const char* kCpuSocket          = "AM4";
constexpr const char* kCpuManufacturer    = "Advanced Micro Devices, Inc.";

// SMBIOS handles.
constexpr uint16_t kHandleBios   = 0x0000;
constexpr uint16_t kHandleSystem = 0x0001;
constexpr uint16_t kHandleBoard  = 0x0002;
constexpr uint16_t kHandleChassis = 0x0003;
constexpr uint16_t kHandleCpu    = 0x0004;
constexpr uint16_t kHandleMemArray = 0x1000;
constexpr uint16_t kHandleDimmBase = 0x1100;
constexpr uint16_t kHandleEnd    = 0x7f00;

// DIMM SPD address -> SMBIOS Device Locator (board silkscreen). Order matches
// i2c-7 0x50..0x53; confirm against board if channel mapping ever looks wrong.
const std::array<const char*, 4> kDimmLocators = {
    "CPU1_DIMM_A1", "CPU1_DIMM_A2", "CPU1_DIMM_B1", "CPU1_DIMM_B2"};

class StructWriter
{
  public:
    StructWriter(uint8_t type, uint16_t handle)
    {
        fmt.push_back(type);
        fmt.push_back(0); // length filled in on flush()
        fmt.push_back(static_cast<uint8_t>(handle & 0xFF));
        fmt.push_back(static_cast<uint8_t>(handle >> 8));
    }
    void u8(uint8_t v) { fmt.push_back(v); }
    void u16(uint16_t v)
    {
        fmt.push_back(static_cast<uint8_t>(v & 0xFF));
        fmt.push_back(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            fmt.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            fmt.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void bytes(size_t n, uint8_t fill = 0) { fmt.insert(fmt.end(), n, fill); }
    // Add a string, return its 1-based index (0 if empty).
    uint8_t str(const std::string& s)
    {
        if (s.empty())
            return 0;
        strings.push_back(s);
        return static_cast<uint8_t>(strings.size());
    }
    void flush(std::vector<uint8_t>& out)
    {
        fmt[1] = static_cast<uint8_t>(fmt.size()); // length = formatted area
        out.insert(out.end(), fmt.begin(), fmt.end());
        if (strings.empty())
        {
            out.push_back(0x00);
            out.push_back(0x00); // empty string-set terminator
        }
        else
        {
            for (const auto& s : strings)
            {
                out.insert(out.end(), s.begin(), s.end());
                out.push_back(0x00);
            }
            out.push_back(0x00); // final double-NUL
        }
    }

  private:
    std::vector<uint8_t> fmt;
    std::vector<std::string> strings;
};

// ---------------------------------------------------------------------------
// FruDevice lookup (best-effort; static fallbacks on any failure).
// ---------------------------------------------------------------------------
struct FruInfo
{
    std::string productManufacturer;
    std::string productName;
    std::string productSerial;
    std::string productPart;
    std::string productVersion;
    std::string boardManufacturer;
    std::string boardName;
    std::string boardSerial;
    std::string boardPart;
};

// Read one FruDevice string property; "" on any failure / non-string type.
std::string getFruStr(sdbusplus::bus_t& bus, const std::string& service,
                      const std::string& path, const char* prop)
{
    try
    {
        auto m = bus.new_method_call(service.c_str(), path.c_str(),
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append("xyz.openbmc_project.FruDevice", prop);
        std::variant<std::string> v;
        bus.call(m).read(v);
        if (const auto* s = std::get_if<std::string>(&v))
            return *s;
    }
    catch (const std::exception&)
    {
        // missing or non-string property -> empty
    }
    return {};
}

FruInfo readFru()
{
    FruInfo fru;
    try
    {
        auto bus = getSdBus();
        auto sub = bus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        sub.append("/xyz/openbmc_project/FruDevice", 0,
                   std::vector<std::string>{"xyz.openbmc_project.FruDevice"});
        std::map<std::string,
                 std::map<std::string, std::vector<std::string>>>
            subtree;
        bus->call(sub).read(subtree);
        if (subtree.empty())
        {
            return fru;
        }
        const auto& [path, svcMap] = *subtree.begin();
        if (svcMap.empty())
        {
            return fru;
        }
        const std::string& service = svcMap.begin()->first;

        fru.productManufacturer = getFruStr(*bus, service, path, "PRODUCT_MANUFACTURER");
        fru.productName         = getFruStr(*bus, service, path, "PRODUCT_PRODUCT_NAME");
        fru.productSerial       = getFruStr(*bus, service, path, "PRODUCT_SERIAL_NUMBER");
        fru.productPart         = getFruStr(*bus, service, path, "PRODUCT_PART_NUMBER");
        fru.productVersion      = getFruStr(*bus, service, path, "PRODUCT_VERSION");
        fru.boardManufacturer   = getFruStr(*bus, service, path, "BOARD_MANUFACTURER");
        fru.boardName           = getFruStr(*bus, service, path, "BOARD_PRODUCT_NAME");
        fru.boardSerial         = getFruStr(*bus, service, path, "BOARD_SERIAL_NUMBER");
        fru.boardPart           = getFruStr(*bus, service, path, "BOARD_PART_NUMBER");
    }
    catch (const std::exception& e)
    {
        log<level::WARNING>("smbiosbuild::readFru: FruDevice unavailable",
                            entry("ERROR=%s", e.what()));
    }
    return fru;
}

std::string firstNonEmpty(const std::string& a, const std::string& b,
                          const char* c)
{
    if (!a.empty())
        return a;
    if (!b.empty())
        return b;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// Table assembly
// ---------------------------------------------------------------------------
std::vector<uint8_t> buildSmbiosTable()
{
    FruInfo fru = readFru();
    auto dimms = spd::readAllDimms();

    std::string sysMfr = firstNonEmpty(fru.productManufacturer,
                                       fru.boardManufacturer, kSysManufacturer);
    std::string sysProd = firstNonEmpty(g_boardName, fru.productName,
                                        kSysProductDefault);
    std::string boardMfr = firstNonEmpty(fru.boardManufacturer,
                                         fru.productManufacturer,
                                         kSysManufacturer);
    std::string boardProd = firstNonEmpty(fru.boardName, g_boardName,
                                          kSysProductDefault);

    std::vector<uint8_t> out;
    out.reserve(2048);

    // ---- Type 0: BIOS Information ----
    {
        StructWriter s(0, kHandleBios);
        uint8_t vendorIdx  = s.str(kBiosVendor);
        uint8_t versionIdx = s.str(g_biosVersion.empty() ? std::string("Unknown")
                                                          : g_biosVersion);
        uint8_t dateIdx    = s.str(g_biosReleaseDate.empty()
                                       ? std::string("01/01/2020")
                                       : g_biosReleaseDate);
        s.u8(vendorIdx);
        s.u8(versionIdx);
        s.u16(0xF000);          // BIOS starting address segment
        s.u8(dateIdx);
        s.u8(0xFF);             // ROM size: (0xFF+1)*64KB = 16 MB
        s.u64(0x0000000000098080ULL); // characteristics: PCI/BIOS upgradeable/etc
        s.u16(0x0007);          // ext characteristics bytes 1-2: UEFI + targeted
        s.u8(2);                // system BIOS major
        s.u8(59);               // system BIOS minor
        s.u8(0xFF);             // embedded controller major (n/a)
        s.u8(0xFF);             // embedded controller minor (n/a)
        s.flush(out);
    }

    // ---- Type 1: System Information ----
    {
        StructWriter s(1, kHandleSystem);
        uint8_t mfr  = s.str(sysMfr);
        uint8_t prod = s.str(sysProd);
        uint8_t ver  = s.str(firstNonEmpty(fru.productVersion, "", "1.0"));
        uint8_t ser  = s.str(fru.productSerial);
        s.u8(mfr);
        s.u8(prod);
        s.u8(ver);
        s.u8(ser);
        s.bytes(16, 0);         // UUID (unknown -> all zero)
        s.u8(0x06);             // wake-up type: Power Switch
        s.u8(0);                // SKU number (none)
        uint8_t fam = s.str("Server");
        s.u8(fam);              // family
        s.flush(out);
    }

    // ---- Type 2: Baseboard Information ----
    {
        StructWriter s(2, kHandleBoard);
        uint8_t mfr  = s.str(boardMfr);
        uint8_t prod = s.str(boardProd);
        uint8_t ver  = s.str("");
        uint8_t ser  = s.str(fru.boardSerial);
        uint8_t asset = s.str("");
        s.u8(mfr);
        s.u8(prod);
        s.u8(ver);
        s.u8(ser);
        s.u8(asset);
        s.u8(0x09);             // feature flags: hosting board + replaceable
        s.u8(0);                // location in chassis (none)
        s.u16(kHandleChassis);  // chassis handle
        s.u8(0x0A);             // board type: Motherboard
        s.u8(0);                // number of contained object handles
        s.flush(out);
    }

    // ---- Type 3: Chassis (minimal, 2.0 length) ----
    {
        StructWriter s(3, kHandleChassis);
        uint8_t mfr = s.str(sysMfr);
        s.u8(mfr);
        s.u8(0x11);             // chassis type: Main Server Chassis
        s.u8(0);                // version
        s.u8(0);                // serial
        s.u8(0);                // asset tag
        s.flush(out);
    }

    // ---- Type 4: Processor Information ----
    {
        StructWriter s(4, kHandleCpu);
        uint8_t socket = s.str(kCpuSocket);
        uint8_t mfr    = s.str(kCpuManufacturer);
        uint8_t ver    = s.str("AMD Ryzen Processor");
        s.u8(socket);
        s.u8(0x03);             // processor type: Central Processor
        s.u8(0x6B);             // family: "AMD Zen" range (best-effort)
        s.u8(mfr);
        s.u64(0);               // processor ID (CPUID -> unknown on BMC)
        s.u8(ver);
        s.u8(0x8A);             // voltage 1.0V indicator
        s.u16(0);               // external clock (unknown)
        s.u16(0);               // max speed (unknown)
        s.u16(0);               // current speed (unknown)
        s.u8(0x41);             // status: populated + enabled
        s.u8(0x02);             // upgrade: unknown
        s.u16(0xFFFF);          // L1 cache handle (none)
        s.u16(0xFFFF);          // L2 cache handle (none)
        s.u16(0xFFFF);          // L3 cache handle (none)
        s.u8(0);                // serial
        s.u8(0);                // asset tag
        s.u8(0);                // part number
        s.u8(0);                // core count (unknown)
        s.u8(0);                // core enabled
        s.u8(0);                // thread count
        s.u16(0x0004);          // characteristics: 64-bit capable
        s.u16(0);               // family 2
        s.u16(0);               // core count 2
        s.u16(0);               // core enabled 2
        s.u16(0);               // thread count 2
        s.flush(out);
    }

    // ---- Type 16: Physical Memory Array ----
    int populated = 0;
    uint64_t totalKb = 0;
    for (const auto& d : dimms)
        if (d.present)
        {
            ++populated;
            totalKb += static_cast<uint64_t>(d.sizeMiB) * 1024;
        }
    bool anyEcc = false;
    for (const auto& d : dimms)
        anyEcc = anyEcc || (d.present && d.ecc);
    {
        StructWriter s(16, kHandleMemArray);
        s.u8(0x03);             // location: System board
        s.u8(0x03);             // use: System memory
        s.u8(anyEcc ? 0x06 : 0x03); // error correction: Multi-bit ECC / None
        // maximum capacity (KB); if > 2 TB use extended field. Board max 128 GB.
        uint64_t maxKb = (totalKb > 0) ? totalKb
                                       : (128ull * 1024 * 1024);
        if (maxKb < 0x80000000ull)
            s.u32(static_cast<uint32_t>(maxKb));
        else
            s.u32(0x80000000);
        s.u16(0xFFFE);          // memory error info handle: not provided
        s.u16(static_cast<uint16_t>(dimms.size())); // number of slots
        s.u64(maxKb < 0x80000000ull ? 0 : maxKb * 1024); // extended max (bytes)
        s.flush(out);
    }

    // ---- Type 17: Memory Device (one per slot) ----
    for (size_t i = 0; i < dimms.size(); ++i)
    {
        const auto& d = dimms[i];
        StructWriter s(17, static_cast<uint16_t>(kHandleDimmBase + i));
        s.u16(kHandleMemArray); // physical memory array handle
        s.u16(0xFFFE);          // memory error info handle: not provided
        uint16_t totalWidth = d.present ? (d.ecc ? 72 : 64) : 0xFFFF;
        uint16_t dataWidth  = d.present ? 64 : 0xFFFF;
        s.u16(totalWidth);
        s.u16(dataWidth);
        // size: MB if < 0x7FFF, else 0x7FFF + extendedSize
        uint32_t mib = d.present ? d.sizeMiB : 0;
        if (!d.present)
            s.u16(0);           // size 0 = slot empty
        else if (mib < 0x7FFF)
            s.u16(static_cast<uint16_t>(mib));
        else
            s.u16(0x7FFF);
        s.u8(0x09);             // form factor: DIMM
        s.u8(0);                // device set
        uint8_t locIdx = s.str(i < kDimmLocators.size() ? kDimmLocators[i]
                                                        : "DIMM");
        uint8_t bankIdx = s.str("P0_Node0");
        s.u8(locIdx);
        s.u8(bankIdx);
        s.u8(d.present ? spd::kSmbiosTypeDDR4 : 0x02); // memory type DDR4/Unknown
        s.u16(d.present ? 0x0080 : 0x0000); // type detail: Synchronous
        s.u16(d.present ? d.speedMTs : 0);  // speed (MT/s)
        uint8_t mfrIdx  = d.present ? s.str(d.manufacturer) : 0;
        uint8_t serIdx  = d.present ? s.str(d.serial) : 0;
        uint8_t assetIdx = 0;
        uint8_t partIdx = d.present ? s.str(d.partNumber) : 0;
        s.u8(mfrIdx);
        s.u8(serIdx);
        s.u8(assetIdx);
        s.u8(partIdx);
        s.u8(d.present ? d.ranks : 0); // attributes: rank in low nibble
        // extended size (MB) when size field saturated
        s.u32((d.present && mib >= 0x7FFF) ? mib : 0);
        s.u16(d.present ? d.speedMTs : 0); // configured memory speed
        s.u16(1200);            // minimum voltage (mV)
        s.u16(1200);            // maximum voltage
        s.u16(1200);            // configured voltage
        s.u8(d.present ? 0x03 : 0x02); // memory technology: DRAM / Unknown
        s.u16(0x0000);          // memory operating mode capability
        s.u8(0);                // firmware version (string ref, none)
        s.u16(d.present ? d.jedecMfrId : 0); // module manufacturer ID
        s.u16(0);               // module product ID
        s.u16(0);               // memory subsystem controller mfr ID
        s.u16(0);               // memory subsystem controller product ID
        s.u64(0);               // non-volatile size
        s.u64(d.present ? static_cast<uint64_t>(mib) * 1024 * 1024 : 0); // volatile
        s.u64(0);               // cache size
        s.u64(0);               // logical size
        s.flush(out);
    }

    // ---- Type 127: End-of-Table ----
    {
        StructWriter s(127, kHandleEnd);
        s.flush(out);
    }

    log<level::INFO>("smbiosbuild::buildSmbiosTable: assembled",
                     entry("BYTES=%zu", out.size()),
                     entry("DIMMS=%d", populated),
                     entry("BIOS=%s", g_biosVersion.c_str()),
                     entry("PRODUCT=%s", sysProd.c_str()));
    return out;
}

} // namespace smbiosbuild
