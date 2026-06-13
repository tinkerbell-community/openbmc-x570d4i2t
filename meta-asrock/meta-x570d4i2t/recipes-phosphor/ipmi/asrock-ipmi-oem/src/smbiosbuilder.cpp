// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// smbiosbuild:: SMBIOS table synthesizer implementation.

#include <smbiosbuilder.hpp>
#include <amiconverter.hpp>

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// Cache: the table is rebuilt (running the expensive SPD/FRU reads) only when a
// host field actually changes. g_dirty starts true so the first build runs.
bool                 g_dirty = true;
std::vector<uint8_t> g_cachedTable;   // smbios-mdr view: [structs][_SM3_ EP]
std::vector<uint8_t> g_cachedBiosView; // BIOS view:       [_SM3_ EP][structs]

// The host only pushes its OEM fields (BIOS version via 0xB2, board name via
// 0xB5) on a COLD boot. On a warm reboot it just reads the table back, and an
// ipmid restart (e.g. a redeploy) would otherwise lose the version -> Type 0
// regresses to "Unknown". Persist the captured fields so they survive both.
constexpr const char* kStateDir  = "/var/lib/asrock-ipmi-oem";
constexpr const char* kStateFile = "/var/lib/asrock-ipmi-oem/hostfields";
bool g_loaded = false;

void persistFields()
{
    try
    {
        std::error_code ec;
        std::filesystem::create_directories(kStateDir, ec);
        std::ofstream f(kStateFile, std::ios::trunc);
        if (!f)
            return;
        // One "key=value" per line; captured values never contain newlines.
        f << "version=" << g_biosVersion << '\n'
          << "date=" << g_biosReleaseDate << '\n'
          << "board=" << g_boardName << '\n';
    }
    catch (const std::exception&)
    {
    }
}

// Seed the in-memory fields from disk once, so a fresh ipmid (or a warm reboot
// with no host push) still builds a table carrying the last-known values. Only
// fills fields that are currently empty — a live host push always wins.
void ensureLoaded()
{
    if (g_loaded)
        return;
    g_loaded = true;
    try
    {
        std::ifstream f(kStateFile);
        if (!f)
            return;
        std::string line;
        while (std::getline(f, line))
        {
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (val.empty())
                continue;
            if (key == "version" && g_biosVersion.empty())
                g_biosVersion = val;
            else if (key == "date" && g_biosReleaseDate.empty())
                g_biosReleaseDate = val;
            else if (key == "board" && g_boardName.empty())
                g_boardName = val;
        }
    }
    catch (const std::exception&)
    {
    }
}

// ---------------------------------------------------------------------------
// DIMM inventory — file-backed, NO i2c.
//
// The custom raw-i2c SPD reader was removed: the host BIOS is a pure consumer
// of the BMC's SMBIOS table (verified by busctl-monitoring a cold boot — the
// host issues only GetBlock reads, never a write), so DIMM detail cannot come
// from the host over IPMI, and live SPD i2c was fragile (the SoC owns the bus
// after memory training). DIMM data now comes solely from a static file written
// out-of-band: `/var/lib/asrock-ipmi-oem/dimms`, one tab-separated record per
// populated slot. If the file is absent the memory slots are emitted as
// present-but-unknown (Type 17 size 0xFFFF). This file is also the intended
// landing spot for a future host-push path.
//
// Record columns (tab-separated):
//   slotIndex  spdAddr  sizeMiB  speedMTs  ranks  deviceWidth  busWidthBits
//   ecc(0/1)   smbiosMemType  jedecMfrId  manufacturer  partNumber  serial
// ---------------------------------------------------------------------------
constexpr const char* kDimmFile = "/var/lib/asrock-ipmi-oem/dimms";
constexpr uint8_t     kSmbiosTypeDDR4 = 0x1A;

struct DimmInfo
{
    uint8_t     slotIndex    = 0;
    uint32_t    sizeMiB      = 0;
    uint16_t    speedMTs     = 0;
    uint8_t     ranks        = 0;
    uint8_t     busWidthBits = 64;
    bool        ecc          = false;
    uint8_t     smbiosMemType = kSmbiosTypeDDR4;
    uint16_t    jedecMfrId   = 0;
    std::string manufacturer;
    std::string partNumber;
    std::string serial;
};

std::map<uint8_t, DimmInfo> loadDimms()
{
    std::map<uint8_t, DimmInfo> out; // keyed by slotIndex
    try
    {
        std::ifstream f(kDimmFile);
        if (!f)
            return out;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty())
                continue;
            std::vector<std::string> c;
            size_t start = 0;
            for (;;)
            {
                size_t tab = line.find('\t', start);
                c.push_back(line.substr(start, tab - start));
                if (tab == std::string::npos)
                    break;
                start = tab + 1;
            }
            if (c.size() < 13)
                continue;
            DimmInfo d;
            d.slotIndex     = static_cast<uint8_t>(std::stoul(c[0]));
            d.sizeMiB       = static_cast<uint32_t>(std::stoul(c[2]));
            d.speedMTs      = static_cast<uint16_t>(std::stoul(c[3]));
            d.ranks         = static_cast<uint8_t>(std::stoul(c[4]));
            d.busWidthBits  = static_cast<uint8_t>(std::stoul(c[6]));
            d.ecc           = (c[7] != "0");
            d.smbiosMemType = static_cast<uint8_t>(std::stoul(c[8]));
            d.jedecMfrId    = static_cast<uint16_t>(std::stoul(c[9]));
            d.manufacturer  = c[10];
            d.partNumber    = c[11];
            d.serial        = c[12];
            out[d.slotIndex] = d;
        }
    }
    catch (const std::exception&)
    {
    }
    return out;
}
} // namespace

void setHostBios(const std::string& version, const std::string& releaseDate)
{
    ensureLoaded();
    bool changed = false;
    if (!version.empty() && version != g_biosVersion)
    {
        g_biosVersion = version;
        g_dirty = true;
        changed = true;
    }
    if (!releaseDate.empty() && releaseDate != g_biosReleaseDate)
    {
        g_biosReleaseDate = releaseDate;
        g_dirty = true;
        changed = true;
    }
    if (changed)
        persistFields();
}

void setHostBoardName(const std::string& productName)
{
    ensureLoaded();
    if (!productName.empty() && productName != g_boardName)
    {
        g_boardName = productName;
        g_dirty = true;
        persistFields();
    }
}

bool needsRebuild()
{
    ensureLoaded();
    return g_dirty || g_cachedTable.empty();
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
constexpr uint16_t kHandleHostIface = 0x4200;
constexpr uint16_t kHandleEnd    = 0x7f00;

// Slot index -> SMBIOS Device Locator (board silkscreen). Slot index is the key
// in the static dims file; A1/A2/B1/B2 = slots 0..3.
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

// Build a 24-byte SMBIOS 3.0 entry point (EntryPointStructure30) with a valid
// checksum. The stock AMI dump uses this exact format (version 3.0, epRev 1).
std::vector<uint8_t> makeSm3EntryPoint(uint32_t structTableLen,
                                       uint64_t structTableAddr)
{
    std::vector<uint8_t> ep;
    const char* anchor = "_SM3_";
    ep.insert(ep.end(), anchor, anchor + 5); // [0-4] "_SM3_"
    ep.push_back(0x00);                       // [5]   checksum (filled below)
    ep.push_back(0x18);                       // [6]   epLength = 24
    ep.push_back(0x03);                       // [7]   SMBIOS major = 3
    ep.push_back(0x00);                       // [8]   SMBIOS minor = 0
    ep.push_back(0x00);                       // [9]   doc rev
    ep.push_back(0x01);                       // [10]  entry point revision
    ep.push_back(0x00);                       // [11]  reserved
    for (int i = 0; i < 4; ++i)               // [12-15] struct table max size
        ep.push_back(static_cast<uint8_t>((structTableLen >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i)               // [16-23] struct table address
        ep.push_back(static_cast<uint8_t>((structTableAddr >> (8 * i)) & 0xFF));
    uint8_t sum = 0;
    for (uint8_t b : ep)
        sum = static_cast<uint8_t>(sum + b);
    ep[5] = static_cast<uint8_t>(0u - sum);   // make the 24-byte sum == 0
    return ep;
}

// Extract the structure table (Type 0 .. Type 127 inclusive) from a payload,
// dropping any trailing entry point. Walks structures to the End-of-Table.
std::vector<uint8_t> extractCore(const std::vector<uint8_t>& payload)
{
    size_t off = 0;
    while (off + 4 <= payload.size())
    {
        uint8_t type = payload[off];
        uint8_t len = payload[off + 1];
        if (len < 4 || off + len > payload.size())
            break;
        size_t p = off + len;
        // skip the string-set (terminated by a double NUL)
        if (p + 1 < payload.size() && payload[p] == 0 && payload[p + 1] == 0)
        {
            p += 2;
        }
        else
        {
            while (p + 1 < payload.size() &&
                   !(payload[p] == 0 && payload[p + 1] == 0))
                ++p;
            p += 2;
        }
        if (type == 127)
            return std::vector<uint8_t>(payload.begin(), payload.begin() + p);
        off = p;
    }
    return payload; // fallback: hand back whatever we got
}

} // namespace

// Build a complete SMBIOS Type 42 (Management Controller Host Interface)
// structure advertising the in-band USB-network (RNDIS) Redfish Host Interface,
// per DMTF DSP0270, so a host OS can auto-discover the BMC over usb0. Returns a
// whole appendable structure: formatted area + terminating double-NULL (no text
// strings). Splice it in immediately before the Type 127 end-of-table struct.
//
// Interface = Network Host Interface (0x40), USB device descriptor for the
// Linux-gadget RNDIS NIC (idVendor 0x1d6b / idProduct 0x0104), one Redfish-
// over-IP protocol record: host 169.254.0.18/16, BMC 169.254.0.17:443.
static std::vector<uint8_t> buildType42HostInterface(uint16_t handle)
{
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
    };
    // DSP0270 stores each IP in a fixed 16-byte field; IPv4 uses the first 4.
    auto putIp4 = [](std::vector<uint8_t>& v, uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d) {
        v.push_back(a);
        v.push_back(b);
        v.push_back(c);
        v.push_back(d);
        v.insert(v.end(), 12, 0);
    };

    std::vector<uint8_t> dev;
    dev.push_back(0x02); // Device Type: USB Network Interface
    put16(dev, 0x1d6b);  // idVendor: Linux Foundation (the gadget)
    put16(dev, 0x0104);  // idProduct: Multifunction/RNDIS gadget

    std::vector<uint8_t> rf;
    rf.insert(rf.end(), 16, 0);  // Redfish Service UUID (unset)
    rf.push_back(0x03);          // Host IP Assignment Type: AutoConfigure
    rf.push_back(0x01);          // Host IP Address Format: IPv4
    putIp4(rf, 169, 254, 0, 18); // Host (BIOS-side) IP
    putIp4(rf, 255, 255, 0, 0);  // Host IP subnet mask
    rf.push_back(0x01);          // Redfish Service IP Discovery Type: Static
    rf.push_back(0x01);          // Redfish Service IP Address Format: IPv4
    putIp4(rf, 169, 254, 0, 17); // Redfish service (BMC) IP
    putIp4(rf, 255, 255, 0, 0);  // Redfish service subnet mask
    put16(rf, 443);              // Redfish Service IP Port
    put32(rf, 0xFFFFFFFF);       // Redfish Service VLAN ID: none
    const std::string svcHost = "169.254.0.17";
    rf.push_back(static_cast<uint8_t>(svcHost.size()));
    rf.insert(rf.end(), svcHost.begin(), svcHost.end());

    std::vector<uint8_t> proto;
    proto.push_back(0x04); // Protocol Type: Redfish over IP
    proto.push_back(static_cast<uint8_t>(rf.size()));
    proto.insert(proto.end(), rf.begin(), rf.end());

    std::vector<uint8_t> s;
    s.push_back(42);                               // Type
    s.push_back(0);                                // Length (filled below)
    put16(s, handle);                              // Handle
    s.push_back(0x40);                             // Network Host Interface
    s.push_back(static_cast<uint8_t>(dev.size())); // interface data length
    s.insert(s.end(), dev.begin(), dev.end());
    s.push_back(1);                                // Number of Protocol Records
    s.insert(s.end(), proto.begin(), proto.end());
    s[1] = static_cast<uint8_t>(s.size());         // formatted-area length
    s.push_back(0);                                // no strings -> double-NULL
    s.push_back(0);
    return s;
}

// ---------------------------------------------------------------------------
// Table assembly
// ---------------------------------------------------------------------------
std::vector<uint8_t> buildSmbiosTable()
{
    // Seed last-known host fields from disk so a fresh ipmid / warm reboot still
    // emits Type 0 BIOS version etc. instead of regressing to "Unknown".
    ensureLoaded();

    // Fast path: nothing changed since the last build — return the cached table
    // (no SPD/FRU I/O). Keeps repeated 0x5D commits cheap and byte-stable.
    if (!g_dirty && !g_cachedTable.empty())
    {
        return g_cachedTable;
    }

    FruInfo fru = readFru();
    auto dimms = loadDimms(); // file-backed, no i2c

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
    int populated = static_cast<int>(dimms.size());
    bool anyEcc = false;
    for (const auto& [slot, d] : dimms)
        anyEcc = anyEcc || d.ecc;
    {
        StructWriter s(16, kHandleMemArray);
        s.u8(0x03);             // location: System board
        s.u8(0x03);             // use: System memory
        s.u8(anyEcc ? 0x06 : 0x03); // error correction: Multi-bit ECC / None
        // maximum capacity: board static max 128 GB (4 x 32 GB UDIMM).
        s.u32(static_cast<uint32_t>(128ull * 1024 * 1024));
        s.u16(0xFFFE);          // memory error info handle: not provided
        s.u16(static_cast<uint16_t>(kDimmLocators.size())); // number of slots
        s.u64(0);               // extended max capacity (n/a, < 2 TB)
        s.flush(out);
    }

    // ---- Type 17: Memory Device (one per physical slot) ----
    // DIMM detail is sourced only from the static dims file (no i2c). The host
    // BIOS does not push DIMM data over IPMI, so a slot absent from the file is
    // emitted as present-but-unknown (size 0xFFFF) rather than fabricated.
    for (size_t i = 0; i < kDimmLocators.size(); ++i)
    {
        auto it = dimms.find(static_cast<uint8_t>(i));
        bool known = (it != dimms.end());
        const DimmInfo* d = known ? &it->second : nullptr;
        StructWriter s(17, static_cast<uint16_t>(kHandleDimmBase + i));
        s.u16(kHandleMemArray); // physical memory array handle
        s.u16(0xFFFE);          // memory error info handle: not provided
        s.u16(known ? (d->ecc ? 72 : 64) : 0xFFFF); // total width
        s.u16(known ? 64 : 0xFFFF);                  // data width
        // size: known -> MB (saturate at 0x7FFF + extended); unknown -> 0xFFFF
        uint32_t mib = known ? d->sizeMiB : 0;
        if (!known)
            s.u16(0xFFFF);      // size unknown
        else if (mib < 0x7FFF)
            s.u16(static_cast<uint16_t>(mib));
        else
            s.u16(0x7FFF);
        s.u8(0x09);             // form factor: DIMM
        s.u8(0);                // device set
        uint8_t locIdx = s.str(kDimmLocators[i]);
        uint8_t bankIdx = s.str("P0_Node0");
        s.u8(locIdx);
        s.u8(bankIdx);
        s.u8(known ? d->smbiosMemType : kSmbiosTypeDDR4); // memory type DDR4
        s.u16(0x0080);          // type detail: Synchronous
        s.u16(known ? d->speedMTs : 0);  // speed (MT/s), 0 = unknown
        uint8_t mfrIdx  = known ? s.str(d->manufacturer) : 0;
        uint8_t serIdx  = known ? s.str(d->serial) : 0;
        uint8_t assetIdx = 0;
        uint8_t partIdx = known ? s.str(d->partNumber) : 0;
        s.u8(mfrIdx);
        s.u8(serIdx);
        s.u8(assetIdx);
        s.u8(partIdx);
        s.u8(known ? d->ranks : 0); // attributes: rank in low nibble
        // extended size (MB) when size field saturated
        s.u32((known && mib >= 0x7FFF) ? mib : 0);
        s.u16(known ? d->speedMTs : 0); // configured memory speed
        s.u16(1200);            // minimum voltage (mV)
        s.u16(1200);            // maximum voltage
        s.u16(1200);            // configured voltage
        s.u8(0x03);             // memory technology: DRAM
        s.u16(0x0000);          // memory operating mode capability
        s.u8(0);                // firmware version (string ref, none)
        s.u16(known ? d->jedecMfrId : 0); // module manufacturer ID
        s.u16(0);               // module product ID
        s.u16(0);               // memory subsystem controller mfr ID
        s.u16(0);               // memory subsystem controller product ID
        s.u64(0);               // non-volatile size
        s.u64(known ? static_cast<uint64_t>(mib) * 1024 * 1024 : 0); // volatile
        s.u64(0);               // cache size
        s.u64(0);               // logical size
        s.flush(out);
    }

    // ---- Type 42: Management Controller Host Interface ----
    // Advertise the usb0 RNDIS Redfish Host Interface so a host OS can
    // auto-discover the BMC at 169.254.0.17:443. Type 42 carries no text
    // strings, so its complete structure (formatted area + double-NULL) is
    // appended directly rather than via StructWriter.
    {
        std::vector<uint8_t> t42 = buildType42HostInterface(kHandleHostIface);
        out.insert(out.end(), t42.begin(), t42.end());
    }

    // ---- Type 127: End-of-Table ----
    {
        StructWriter s(127, kHandleEnd);
        s.flush(out);
    }

    // `out` is now the core structure table (Type 0 .. Type 127). Build the two
    // views the two consumers need from it:
    //
    //  - smbios-mdr (the /var/lib/smbios/smbios2 file): structures at offset 0
    //    (getSMBIOSTypePtr walks from there) + a "_SM3_" anchor APPENDED so
    //    checkSMBIOSVersion's whole-buffer search still finds a version.
    //
    //  - the AMI host BIOS (read back via 0x72 GetBlock): a STANDARD SMBIOS dump
    //    with the "_SM3_" entry point FIRST, then the structures — byte-for-byte
    //    the shape the stock firmware served, which the BIOS validates on POST.
    std::vector<uint8_t> core = out;
    uint32_t coreLen = static_cast<uint32_t>(core.size());

    // smbios-mdr view: core + trailing entry point.
    auto trailingEp = makeSm3EntryPoint(coreLen, /*addr*/ 0);
    out.insert(out.end(), trailingEp.begin(), trailingEp.end());

    // BIOS view: entry point (structs follow it at offset 24) + core.
    std::vector<uint8_t> biosView = makeSm3EntryPoint(coreLen, /*addr*/ 0x18);
    biosView.insert(biosView.end(), core.begin(), core.end());

    log<level::INFO>("smbiosbuild::buildSmbiosTable: assembled",
                     entry("MDR_BYTES=%zu", out.size()),
                     entry("BIOS_BYTES=%zu", biosView.size()),
                     entry("DIMMS=%d", populated),
                     entry("BIOS=%s", g_biosVersion.c_str()),
                     entry("PRODUCT=%s", sysProd.c_str()));

    // Cache and mark clean so subsequent commits fast-path until a field changes.
    // DIMM detail is now file-backed (deterministic) rather than read from flaky
    // i2c, so there is no empty-SPD regression to guard against: always cache.
    g_cachedTable = out;
    g_cachedBiosView = biosView;
    g_dirty = false;
    return out;
}

std::vector<uint8_t> getBiosMdrView()
{
    if (!g_cachedBiosView.empty())
    {
        return g_cachedBiosView;
    }
    // Cold cache (e.g. the BIOS reads the table back before its first push, or
    // ipmid just restarted). Derive the BIOS view from the on-disk smbios2: take
    // its structure table and prepend a "_SM3_" entry point.
    auto core = extractCore(ami::loadMdrPayload());
    if (core.empty())
    {
        return {};
    }
    std::vector<uint8_t> view =
        makeSm3EntryPoint(static_cast<uint32_t>(core.size()), 0x18);
    view.insert(view.end(), core.begin(), core.end());
    g_cachedBiosView = view;
    return view;
}

} // namespace smbiosbuild
