// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// App NetFn (0x06) command overrides for the X570D4I-2T.
//
// Overrides phosphor-ipmi-host defaults at prioOemBase so that:
//   - GetDeviceId (0x01) returns the correct ASRock manufacturer ID / product ID
//     from dev_id.json plus the actual running firmware version string from D-Bus.
//   - GetSystemGuid (0x34) returns the board UUID sourced from the D-Bus
//     inventory UUID property rather than a synthesised value.
//
// Method references:
//   bmc-analyze §NETFN_APP table (g_App_CmdHndlr, confirmed codes 0x01/0x08/0x34)
//   bmc-analyze §Core IPMI Daemon: IPMI.conf (manuf_id=0x00C1D6, prod_id=0x1003)
//   bmc-analyze §Firmware Identity: device ID 0x20 / revision 0x81

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <fstream>
#include <regex>
#include <string>
#include <variant>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

// Dev ID JSON written by the phosphor-ipmi-config recipe
static constexpr const char* devIdJsonPath =
    "/usr/share/ipmi-providers/dev_id.json";

// D-Bus path for BMC chassis object UUID (from hardware-inventory doc)
static constexpr const char* chassisSystemObj =
    "/xyz/openbmc_project/inventory/system";
static constexpr const char* uuidIntf =
    "xyz.openbmc_project.Common.UUID";

// D-Bus Software activation interface (for FW version)
static constexpr const char* softwareRoot = "/xyz/openbmc_project/software";
static constexpr const char* softwareVerIntf =
    "xyz.openbmc_project.Software.Version";
static constexpr const char* softwareActIntf =
    "xyz.openbmc_project.Software.Activation";

// IPMI 2.0 messaging spec version byte
static constexpr uint8_t ipmiMsgSpecVer = 0x20;

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static void registerAppCommands() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Parse "v3.2.0-10-gabcdef" → {major=3, minorBcd=0x02}
static void parseFwVersion(const std::string& verStr,
                           uint8_t& majorOut, uint8_t& minorBcdOut)
{
    // Default to 0.0 on parse failure
    majorOut    = 0;
    minorBcdOut = 0;

    // Match optional leading 'v', then major.minor[.patch...]
    std::regex re(R"([vV]?(\d+)\.(\d+))");
    std::smatch m;
    if (std::regex_search(verStr, m, re))
    {
        try
        {
            uint32_t maj = static_cast<uint32_t>(std::stoul(m[1].str()));
            uint32_t min = static_cast<uint32_t>(std::stoul(m[2].str()));
            majorOut    = static_cast<uint8_t>(maj & 0x7F); // bit7 = update flag
            // BCD encode minor: only two digits max
            uint8_t tens = static_cast<uint8_t>((min / 10) & 0xF);
            uint8_t ones = static_cast<uint8_t>((min % 10) & 0xF);
            minorBcdOut = static_cast<uint8_t>((tens << 4) | ones);
        }
        catch (...) {}
    }
}

// Fetch the active BMC firmware version from D-Bus Software objects.
// Returns an empty string on failure; caller falls back to dev_id.json aux.
static std::string getActiveBmcVersion()
{
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper",
            "GetSubTreePaths");
        msg.append(softwareRoot, 0,
                   std::vector<std::string>{softwareVerIntf});
        auto reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        for (const auto& path : paths)
        {
            try
            {
                std::string svc =
                    ipmi::getService(*dbus, softwareVerIntf, path);

                ipmi::Value actV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareActIntf, "Activation");
                const auto& actStr = std::get<std::string>(actV);
                if (actStr.find("Active") == std::string::npos)
                {
                    continue;
                }
                ipmi::Value verV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareVerIntf, "Version");
                return std::get<std::string>(verV);
            }
            catch (...) { continue; }
        }
    }
    catch (...) {}
    return {};
}

// -----------------------------------------------------------------------
// GetDeviceId (NetFn App / 0x01)
// Confirmed: bmc-analyze §NETFN_APP, §Core IPMI Daemon IPMI.conf
// bmc-analyze §Firmware Identity: manuf_id=0x00C1D6, prod_id=0x1003,
//   device_id=0x20, device_revision=0x81
//
// Response layout (IPMI spec §20.1):
//   [0] deviceId
//   [1] deviceRevision  (bit7 = SDR present)
//   [2] fwMajor         (bit7 = update in progress)
//   [3] fwMinorBcd
//   [4] ipmiVersion     (0x20)
//   [5] additionalSupport
//   [6-8]  manufacturerId (LE 3 bytes)
//   [9-10] productId    (LE 2 bytes)
//   [11-14] auxiliaryFw (optional, 4 bytes LE)
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t,  // deviceId
              uint8_t,  // deviceRevision
              uint8_t,  // fwMajor
              uint8_t,  // fwMinorBcd
              uint8_t,  // ipmiVersion
              uint8_t,  // additionalSupport
              uint8_t, uint8_t, uint8_t,   // manufacturerId[3]
              uint8_t, uint8_t,             // productId[2]
              uint32_t>                     // auxiliaryFw
    ipmiGetDeviceId(ipmi::Context::ptr& /*ctx*/)
{
    // ---- Static fields from dev_id.json ----
    uint8_t  deviceId         = 0x20;
    uint8_t  deviceRevision   = 0x81; // SDR present | revision 1
    uint8_t  ipmiVersion      = ipmiMsgSpecVer;
    uint8_t  additionalSupport = 0xBF; // addn_dev_support from JSON
    // Manufacturer ID: 49622 = 0x00C1D6
    uint8_t  mfgByte0 = 0xD6;
    uint8_t  mfgByte1 = 0xC1;
    uint8_t  mfgByte2 = 0x00;
    // Product ID: 4099 = 0x1003
    uint8_t  prodByte0 = 0x03;
    uint8_t  prodByte1 = 0x10;
    uint32_t auxFw     = 0;

    // Override statics from dev_id.json if present
    try
    {
        std::ifstream ifs(devIdJsonPath);
        if (ifs.is_open())
        {
            nlohmann::json j;
            ifs >> j;
            deviceId          = j.value("id",           deviceId);
            deviceRevision    = static_cast<uint8_t>(
                                    j.value("revision", 1) & 0x0F) |
                                    (deviceRevision & 0xF0);
            additionalSupport = j.value("addn_dev_support", additionalSupport);
            uint32_t mfgId    = j.value("manuf_id", static_cast<uint32_t>(
                                    (mfgByte2 << 16) | (mfgByte1 << 8) | mfgByte0));
            mfgByte0 = static_cast<uint8_t>(mfgId & 0xFF);
            mfgByte1 = static_cast<uint8_t>((mfgId >> 8) & 0xFF);
            mfgByte2 = static_cast<uint8_t>((mfgId >> 16) & 0xFF);
            uint32_t prodId   = j.value("prod_id", static_cast<uint32_t>(
                                    (prodByte1 << 8) | prodByte0));
            prodByte0 = static_cast<uint8_t>(prodId & 0xFF);
            prodByte1 = static_cast<uint8_t>((prodId >> 8) & 0xFF);
            auxFw             = j.value("aux", auxFw);
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetDeviceId: dev_id.json parse failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // ---- Dynamic firmware version from D-Bus ----
    uint8_t fwMajor    = 0;
    uint8_t fwMinorBcd = 0;
    std::string ver = getActiveBmcVersion();
    if (!ver.empty())
    {
        parseFwVersion(ver, fwMajor, fwMinorBcd);
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetDeviceId",
        phosphor::logging::entry("FW_MAJOR=%u", fwMajor),
        phosphor::logging::entry("FW_MINOR_BCD=0x%02X", fwMinorBcd));
    return ipmi::responseSuccess(
        deviceId, deviceRevision,
        fwMajor, fwMinorBcd, ipmiVersion, additionalSupport,
        mfgByte0, mfgByte1, mfgByte2,
        prodByte0, prodByte1,
        auxFw);
}

// -----------------------------------------------------------------------
// GetSystemGuid (NetFn App / 0x34)
// Also handles the older GetDevGuid alias (0x08).
// Confirmed: bmc-analyze §NETFN_APP (0x08 GetDevGuid, 0x34 GetSystemGuid)
//
// Response: 16 bytes of UUID in RFC4122 network byte order
//   (hyphenated string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" → 16 raw bytes)
// -----------------------------------------------------------------------

ipmi::RspType<std::array<uint8_t, 16>>
    ipmiGetSystemGuid(ipmi::Context::ptr& /*ctx*/)
{
    std::array<uint8_t, 16> uuid{};

    // UUID from hardware inventory: 7533E379-96C4-49D9-F452-9C6B0070598A
    // Filled from D-Bus; static fallback from discovered hardware identity.
    std::string uuidStr;

    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, uuidIntf, chassisSystemObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, chassisSystemObj,
                                               uuidIntf, "UUID");
        uuidStr = std::get<std::string>(v);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetSystemGuid: D-Bus UUID lookup failed, using fallback",
            phosphor::logging::entry("ERROR=%s", e.what()));
        // Fallback: board UUID from hardware inventory discovery
        uuidStr = "7533E379-96C4-49D9-F452-9C6B0070598A";
    }

    // Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" → 16 bytes
    // Remove hyphens and parse hex pairs
    std::string hexOnly;
    hexOnly.reserve(32);
    for (char c : uuidStr)
    {
        if (c != '-') hexOnly += c;
    }
    if (hexOnly.size() == 32)
    {
        for (size_t i = 0; i < 16; ++i)
        {
            uuid[i] = static_cast<uint8_t>(
                std::stoul(hexOnly.substr(i * 2, 2), nullptr, 16));
        }
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSystemGuid",
        phosphor::logging::entry("UUID=%s", uuidStr.c_str()));
    return ipmi::responseSuccess(uuid);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerAppCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock App commands registered (NetFn 0x06)");

    // GetDeviceId — override phosphor default to inject real FW version
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnApp,
                          static_cast<ipmi::Cmd>(0x01),
                          ipmi::Privilege::User, ipmiGetDeviceId);

    // GetSystemGuid (0x34)
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnApp,
                          static_cast<ipmi::Cmd>(0x34),
                          ipmi::Privilege::User, ipmiGetSystemGuid);

    // GetDevGuid (0x08) — older alias for GetSystemGuid
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnApp,
                          static_cast<ipmi::Cmd>(0x08),
                          ipmi::Privilege::User, ipmiGetSystemGuid);
}

} // namespace asrock
