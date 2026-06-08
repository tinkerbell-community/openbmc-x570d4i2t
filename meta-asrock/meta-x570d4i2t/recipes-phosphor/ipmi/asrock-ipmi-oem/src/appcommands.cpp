// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// App NetFn (0x06) command overrides for the X570D4I-2T.
//
// Overrides phosphor-ipmi-host defaults at prioOemBase so that:
//   - GetDeviceId (0x01) returns the correct ASRock manufacturer ID / product ID
//     from dev_id.json plus the actual running firmware version string from D-Bus.
//   - GetSelfTestResults (0x04) checks SEL and SDR service reachability.
//   - GetSystemGuid (0x37) returns the board UUID from D-Bus inventory.
//   - GetDevGuid (0x08) is an older alias for GetSystemGuid.
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

static constexpr const char* devIdJsonPath =
    "/usr/share/ipmi-providers/dev_id.json";

static constexpr const char* chassisSystemObj =
    "/xyz/openbmc_project/inventory/system";
static constexpr const char* uuidIntf = "xyz.openbmc_project.Common.UUID";

static constexpr const char* softwareRoot = "/xyz/openbmc_project/software";
static constexpr const char* softwareVerIntf =
    "xyz.openbmc_project.Software.Version";
static constexpr const char* softwareActIntf =
    "xyz.openbmc_project.Software.Activation";

static constexpr uint8_t ipmiMsgSpecVer = 0x20;

static constexpr uint8_t selfTestPass   = 0x55;
static constexpr uint8_t selfTestErrSEL = (1 << 7);
static constexpr uint8_t selfTestErrSDR = (1 << 6);

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static void registerAppCommands() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Parse "v3.2.0-10-gabcdef" → {major=3, minorBcd=0x02}
static void parseFwVersion(const std::string& verStr, uint8_t& majorOut,
                           uint8_t& minorBcdOut)
{
    majorOut    = 0;
    minorBcdOut = 0;

    std::regex  re(R"([vV]?(\d+)\.(\d+))");
    std::smatch m;
    if (std::regex_search(verStr, m, re))
    {
        try
        {
            uint32_t maj = static_cast<uint32_t>(std::stoul(m[1].str()));
            uint32_t min = static_cast<uint32_t>(std::stoul(m[2].str()));
            majorOut         = static_cast<uint8_t>(maj & 0x7F);
            uint8_t tens     = static_cast<uint8_t>((min / 10) & 0xF);
            uint8_t ones     = static_cast<uint8_t>((min % 10) & 0xF);
            minorBcdOut      = static_cast<uint8_t>((tens << 4) | ones);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "parseFwVersion: parsed",
                phosphor::logging::entry("INPUT=%s", verStr.c_str()),
                phosphor::logging::entry("MAJOR=%u", majorOut),
                phosphor::logging::entry("MINOR_BCD=0x%02X", minorBcdOut));
        }
        catch (const std::exception& e)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "parseFwVersion: parse exception",
                phosphor::logging::entry("INPUT=%s", verStr.c_str()),
                phosphor::logging::entry("ERROR=%s", e.what()));
        }
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "parseFwVersion: regex did not match",
            phosphor::logging::entry("INPUT=%s", verStr.c_str()));
    }
}

// Fetch the active BMC firmware version from D-Bus Software objects.
static std::string getActiveBmcVersion()
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "getActiveBmcVersion: querying ObjectMapper",
        phosphor::logging::entry("ROOT=%s", softwareRoot));
    try
    {
        auto dbus = getSdBus();
        auto msg  = dbus->new_method_call("xyz.openbmc_project.ObjectMapper",
                                          "/xyz/openbmc_project/object_mapper",
                                          "xyz.openbmc_project.ObjectMapper",
                                          "GetSubTreePaths");
        msg.append(softwareRoot, 0, std::vector<std::string>{softwareVerIntf});
        auto                     reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "getActiveBmcVersion: software paths found",
            phosphor::logging::entry("COUNT=%zu", paths.size()));

        for (const auto& path : paths)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "getActiveBmcVersion: checking path",
                phosphor::logging::entry("PATH=%s", path.c_str()));
            try
            {
                std::string svc =
                    ipmi::getService(*dbus, softwareVerIntf, path);

                ipmi::Value actV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareActIntf, "Activation");
                const auto& actStr = std::get<std::string>(actV);

                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    "getActiveBmcVersion: activation state",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("STATE=%s", actStr.c_str()));

                if (actStr.find("Active") == std::string::npos)
                {
                    phosphor::logging::log<phosphor::logging::level::DEBUG>(
                        "getActiveBmcVersion: skipping inactive entry",
                        phosphor::logging::entry("PATH=%s", path.c_str()));
                    continue;
                }

                ipmi::Value verV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareVerIntf, "Version");
                std::string ver = std::get<std::string>(verV);

                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "getActiveBmcVersion: found active version",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("VERSION=%s", ver.c_str()));
                return ver;
            }
            catch (const std::exception& e)
            {
                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    "getActiveBmcVersion: error checking path",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("ERROR=%s", e.what()));
                continue;
            }
        }

        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "getActiveBmcVersion: no active software entry found");
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "getActiveBmcVersion: ObjectMapper query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }
    return {};
}

// -----------------------------------------------------------------------
// GetDeviceId (NetFn App / 0x01)
// Confirmed: bmc-analyze §NETFN_APP, §Core IPMI Daemon IPMI.conf
// bmc-analyze §Firmware Identity: manuf_id=0x00C1D6, prod_id=0x1003,
//   device_id=0x20, device_revision=0x81
//
// Response layout (IPMI spec §20.1):
//   [0]    deviceId
//   [1]    deviceRevision  (bit7 = SDR present)
//   [2]    fwMajor         (bit7 = update in progress)
//   [3]    fwMinorBcd
//   [4]    ipmiVersion     (0x20)
//   [5]    additionalSupport
//   [6-8]  manufacturerId  (LE 3 bytes)
//   [9-10] productId       (LE 2 bytes)
//   [11-14] auxiliaryFw   (optional, 4 bytes LE)
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t,                    // deviceId
              uint8_t,                    // deviceRevision
              uint8_t,                    // fwMajor
              uint8_t,                    // fwMinorBcd
              uint8_t,                    // ipmiVersion
              uint8_t,                    // additionalSupport
              uint8_t, uint8_t, uint8_t,  // manufacturerId[3]
              uint8_t, uint8_t,           // productId[2]
              uint32_t>                   // auxiliaryFw
ipmiGetDeviceId(ipmi::Context::ptr& /*ctx*/)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetDeviceId: handler entered");

    // Defaults from IPMI.conf / bmc-analyze
    uint8_t  deviceId         = 0x20;
    uint8_t  deviceRevision   = 0x81; // SDR present | revision 1
    uint8_t  ipmiVersion      = ipmiMsgSpecVer;
    uint8_t  additionalSupport = 0xBF;
    uint8_t  mfgByte0         = 0xD6; // manuf_id 0x00C1D6 LE
    uint8_t  mfgByte1         = 0xC1;
    uint8_t  mfgByte2         = 0x00;
    uint8_t  prodByte0        = 0x03; // prod_id 0x1003 LE
    uint8_t  prodByte1        = 0x10;
    uint32_t auxFw            = 0;

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetDeviceId: defaults loaded",
        phosphor::logging::entry("DEVICE_ID=0x%02X", deviceId),
        phosphor::logging::entry("DEV_REV=0x%02X", deviceRevision),
        phosphor::logging::entry("MFG=0x%02X%02X%02X", mfgByte2, mfgByte1, mfgByte0),
        phosphor::logging::entry("PROD=0x%02X%02X", prodByte1, prodByte0),
        phosphor::logging::entry("ADDN_SUPPORT=0x%02X", additionalSupport));

    // Override statics from dev_id.json if present
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetDeviceId: loading dev_id.json",
        phosphor::logging::entry("PATH=%s", devIdJsonPath));
    try
    {
        std::ifstream ifs(devIdJsonPath);
        if (!ifs.is_open())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "GetDeviceId: dev_id.json not found, using hardcoded defaults",
                phosphor::logging::entry("PATH=%s", devIdJsonPath));
        }
        else
        {
            nlohmann::json j;
            ifs >> j;
            deviceId         = j.value("id", deviceId);
            deviceRevision   = static_cast<uint8_t>(
                                   j.value("revision", 1) & 0x0F) |
                               (deviceRevision & 0xF0);
            additionalSupport = j.value("addn_dev_support", additionalSupport);
            uint32_t mfgId   = j.value(
                "manuf_id",
                static_cast<uint32_t>((mfgByte2 << 16) | (mfgByte1 << 8) |
                                      mfgByte0));
            mfgByte0         = static_cast<uint8_t>(mfgId & 0xFF);
            mfgByte1         = static_cast<uint8_t>((mfgId >> 8) & 0xFF);
            mfgByte2         = static_cast<uint8_t>((mfgId >> 16) & 0xFF);
            uint32_t prodId  = j.value(
                "prod_id",
                static_cast<uint32_t>((prodByte1 << 8) | prodByte0));
            prodByte0        = static_cast<uint8_t>(prodId & 0xFF);
            prodByte1        = static_cast<uint8_t>((prodId >> 8) & 0xFF);
            auxFw            = j.value("aux", auxFw);

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "GetDeviceId: dev_id.json applied",
                phosphor::logging::entry("DEVICE_ID=0x%02X", deviceId),
                phosphor::logging::entry("DEV_REV=0x%02X", deviceRevision),
                phosphor::logging::entry("MFG=0x%02X%02X%02X", mfgByte2,
                                         mfgByte1, mfgByte0),
                phosphor::logging::entry("PROD=0x%02X%02X", prodByte1,
                                         prodByte0),
                phosphor::logging::entry("AUX_FW=0x%08X", auxFw),
                phosphor::logging::entry("ADDN_SUPPORT=0x%02X",
                                         additionalSupport));
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetDeviceId: dev_id.json parse failed, using defaults",
            phosphor::logging::entry("PATH=%s", devIdJsonPath),
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // Dynamic firmware version from D-Bus
    uint8_t     fwMajor    = 0;
    uint8_t     fwMinorBcd = 0;
    std::string ver        = getActiveBmcVersion();
    if (!ver.empty())
    {
        parseFwVersion(ver, fwMajor, fwMinorBcd);
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetDeviceId: firmware version resolved",
            phosphor::logging::entry("VER_STRING=%s", ver.c_str()),
            phosphor::logging::entry("FW_MAJOR=0x%02X", fwMajor),
            phosphor::logging::entry("FW_MINOR_BCD=0x%02X", fwMinorBcd));
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetDeviceId: no active BMC version on D-Bus, FW bytes = 0x00/0x00");
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetDeviceId: responding",
        phosphor::logging::entry("DEVICE_ID=0x%02X", deviceId),
        phosphor::logging::entry("DEV_REV=0x%02X", deviceRevision),
        phosphor::logging::entry("FW_MAJOR=0x%02X", fwMajor),
        phosphor::logging::entry("FW_MINOR_BCD=0x%02X", fwMinorBcd),
        phosphor::logging::entry("IPMI_VER=0x%02X", ipmiVersion),
        phosphor::logging::entry("ADDN_SUPPORT=0x%02X", additionalSupport),
        phosphor::logging::entry("MFG=0x%02X%02X%02X", mfgByte2, mfgByte1,
                                 mfgByte0),
        phosphor::logging::entry("PROD=0x%02X%02X", prodByte1, prodByte0),
        phosphor::logging::entry("AUX_FW=0x%08X", auxFw));

    return ipmi::responseSuccess(deviceId, deviceRevision, fwMajor, fwMinorBcd,
                                 ipmiVersion, additionalSupport, mfgByte0,
                                 mfgByte1, mfgByte2, prodByte0, prodByte1,
                                 auxFw);
}

// -----------------------------------------------------------------------
// GetSelfTestResults (NetFn App / 0x04)
// IPMI 2.0 §20.4
//
// Result byte 1:
//   0x55 = no errors, all self-tests passed
//   0x56 = not implemented
//   0x57 = error(s) — see bit-field in byte 2
//   0xFF = BMC currently in self-test
//
// Result byte 2 (only meaningful when byte 1 = 0x57):
//   bit7 = cannot access SEL device
//   bit6 = cannot access SDR repository
//   bit5 = cannot access BMC FRU device
//   bit3 = SDR repository empty
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint8_t>
ipmiGetSelfTestResults(ipmi::Context::ptr& /*ctx*/)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSelfTestResults: handler entered");

    uint8_t result  = selfTestPass;
    uint8_t errBits = 0;

    // Check SEL service is reachable
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSelfTestResults: checking SEL service");
    try
    {
        auto        dbus = getSdBus();
        std::string svc  = ipmi::getService(
            *dbus, "xyz.openbmc_project.Logging.IPMI",
            "/xyz/openbmc_project/Logging/IPMI");
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetSelfTestResults: SEL service reachable",
            phosphor::logging::entry("SVC=%s", svc.c_str()));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetSelfTestResults: SEL service unreachable — setting bit7",
            phosphor::logging::entry("ERROR=%s", e.what()));
        errBits |= selfTestErrSEL;
    }

    // Check EntityManager (SDR source) is reachable
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSelfTestResults: checking EntityManager (SDR source)");
    try
    {
        auto        dbus = getSdBus();
        std::string svc  = ipmi::getService(
            *dbus, "xyz.openbmc_project.EntityManager",
            "/xyz/openbmc_project/EntityManager");
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetSelfTestResults: EntityManager reachable",
            phosphor::logging::entry("SVC=%s", svc.c_str()));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetSelfTestResults: EntityManager unreachable — setting bit6",
            phosphor::logging::entry("ERROR=%s", e.what()));
        errBits |= selfTestErrSDR;
    }

    if (errBits != 0)
    {
        result = 0x57;
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetSelfTestResults: self-test FAILED",
            phosphor::logging::entry("RESULT=0x%02X", result),
            phosphor::logging::entry("ERR_BITS=0x%02X", errBits),
            phosphor::logging::entry("SEL_ERR=%u", !!(errBits & selfTestErrSEL)),
            phosphor::logging::entry("SDR_ERR=%u",
                                     !!(errBits & selfTestErrSDR)));
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetSelfTestResults: self-test PASSED",
            phosphor::logging::entry("RESULT=0x%02X", result));
    }

    return ipmi::responseSuccess(result, errBits);
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSystemGuid: handler entered",
        phosphor::logging::entry("DBUS_OBJ=%s", chassisSystemObj),
        phosphor::logging::entry("DBUS_INTF=%s", uuidIntf));

    std::array<uint8_t, 16> uuid{};
    std::string             uuidStr;
    bool                    fromDbus = false;

    try
    {
        auto        dbus = getSdBus();
        std::string svc  = ipmi::getService(*dbus, uuidIntf, chassisSystemObj);
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetSystemGuid: UUID service found",
            phosphor::logging::entry("SVC=%s", svc.c_str()));

        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, chassisSystemObj,
                                              uuidIntf, "UUID");
        uuidStr       = std::get<std::string>(v);
        fromDbus      = true;

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetSystemGuid: UUID from D-Bus",
            phosphor::logging::entry("UUID=%s", uuidStr.c_str()));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetSystemGuid: D-Bus UUID lookup failed, using static fallback",
            phosphor::logging::entry("OBJ=%s", chassisSystemObj),
            phosphor::logging::entry("ERROR=%s", e.what()));
        uuidStr  = "7533E379-96C4-49D9-F452-9C6B0070598A";
        fromDbus = false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSystemGuid: parsing UUID string",
        phosphor::logging::entry("UUID_STR=%s", uuidStr.c_str()),
        phosphor::logging::entry("FROM_DBUS=%u", fromDbus));

    // Strip hyphens and decode 16 hex pairs
    std::string hexOnly;
    hexOnly.reserve(32);
    for (char c : uuidStr)
    {
        if (c != '-')
            hexOnly += c;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSystemGuid: hex string after strip",
        phosphor::logging::entry("HEX=%s", hexOnly.c_str()),
        phosphor::logging::entry("LEN=%zu", hexOnly.size()));

    if (hexOnly.size() == 32)
    {
        for (size_t i = 0; i < 16; ++i)
        {
            uuid[i] = static_cast<uint8_t>(
                std::stoul(hexOnly.substr(i * 2, 2), nullptr, 16));
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetSystemGuid: responding",
            phosphor::logging::entry("UUID=%s", uuidStr.c_str()),
            phosphor::logging::entry(
                "BYTES=%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                "%02X%02X%02X%02X%02X%02X",
                uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
                uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12],
                uuid[13], uuid[14], uuid[15]));
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "GetSystemGuid: malformed UUID string — returning all-zeros",
            phosphor::logging::entry("UUID_STR=%s", uuidStr.c_str()),
            phosphor::logging::entry("HEX_LEN=%zu", hexOnly.size()));
    }

    return ipmi::responseSuccess(uuid);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerAppCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock App commands registered (NetFn 0x06)");

    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnApp,
                          ipmi::app::cmdGetDeviceId, ipmi::Privilege::User,
                          ipmiGetDeviceId);

    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnApp,
                          ipmi::app::cmdGetSelfTestResults,
                          ipmi::Privilege::User, ipmiGetSelfTestResults);

    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnApp,
                          ipmi::app::cmdGetSystemGuid, ipmi::Privilege::User,
                          ipmiGetSystemGuid);

    // GetDevGuid (0x08) — older alias, same response as GetSystemGuid
    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnApp,
                          ipmi::app::cmdGetDeviceGuid, ipmi::Privilege::User,
                          ipmiGetSystemGuid);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Registering GetSelfTestResults and GetSystemGuid handlers");
}

} // namespace asrock
