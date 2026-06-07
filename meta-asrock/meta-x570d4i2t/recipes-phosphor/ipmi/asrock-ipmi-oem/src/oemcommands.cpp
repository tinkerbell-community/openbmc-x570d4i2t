// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI command handlers for the X570D4I-2T (NetFn 0x30).
//
// This file mirrors the structure of intel-ipmi-oem/src/oemcommands.cpp
// while replacing Intel-specific NetFn assignments and business logic
// with the Megarac/AMI equivalents extracted from the v01.91.00 firmware
// (see bmc-analyze.instructions.md and megarac-bios-ipmi-methods.instructions.md).
//
// Implemented handlers (all on NetFn 0x30 / NETFN_AMI):
//
//   cmdGetInventory  (0xE6) — board/system inventory from D-Bus FRU + Software objects
//   cmdGetSensorInfo (0x1E) — sensor name/type list from D-Bus dbus-sensors
//   cmdGetFwVersion  (0x20) — BMC firmware version from D-Bus Software objects
//   cmdGetFwProtocol (0x21) — static protocol version echo
//   cmdMuxSwitching  (0xEE) — KVM/SPI mux via GPIOJ1 (line 73)
//   cmdPeciReadWrite (0xE9) — stub; PECI not yet exposed on this platform
//   cmdPsuInfo       (0xEC) — PSU present/online status via D-Bus
//   cmdManageBmcConfig (0x2B) — BMC cold/warm reset via D-Bus
//   cmdGetSelPolicy  (0x30) — SEL policy stub (always returns 0)
//
//   YAFU stubs (0x01–0x10) — return ipmi::responseInvalidCommand() so the
//       host does not receive "invalid command" when probing for YAFU; the
//       actual firmware-upload path uses phosphor-ipmi-blobs.

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus constants
// -----------------------------------------------------------------------

static constexpr const char* boardObjPath  =
    "/xyz/openbmc_project/inventory/system/board";
static constexpr const char* itemBoardIntf =
    "xyz.openbmc_project.Inventory.Item.Board";
static constexpr const char* assetIntf     =
    "xyz.openbmc_project.Inventory.Decorator.Asset";

static constexpr const char* softwareRoot  = "/xyz/openbmc_project/software";
static constexpr const char* softwareIntf  =
    "xyz.openbmc_project.Software.Version";
static constexpr const char* activationIntf =
    "xyz.openbmc_project.Software.Activation";

static constexpr const char* sensorRoot    =
    "/xyz/openbmc_project/sensors";

static constexpr const char* psuService    =
    "xyz.openbmc_project.PSUSensor";
static constexpr const char* psuRoot       =
    "/xyz/openbmc_project/sensors/power";
static constexpr const char* psuItemIntf   =
    "xyz.openbmc_project.Inventory.Item.PowerSupply";
static constexpr const char* operStateIntf =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";

// GPIO line for the KVM/SPI mux — GPIOJ1 on the AST2500
// (bmc-analyze §Critical OEM Commands, VGA/KVM fix memory)
static constexpr unsigned int muxGpioLine = 73;

// Protocol version returned by cmdGetFwProtocol (arbitrary constant matching
// the Megarac firmware handshake the BIOS expects: 1 = "OOB supported")
static constexpr uint8_t oemProtocolVersion = 0x01;

// -----------------------------------------------------------------------
// Forward declaration
// -----------------------------------------------------------------------

static void registerOEMFunctions() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helper: resolve D-Bus service name for a given interface + object path
// -----------------------------------------------------------------------

static std::string getService(const std::string& intf,
                               const std::string& objPath)
{
    try
    {
        auto dbus = getSdBus();
        return ipmi::getService(*dbus, intf, objPath);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "getService: failed",
            phosphor::logging::entry("INTF=%s", intf.c_str()),
            phosphor::logging::entry("ERROR=%s", e.what()));
        return {};
    }
}

// -----------------------------------------------------------------------
// cmdGetInventory (0xE6) — board/system inventory from D-Bus
// Confirmed: g_AMI_CmdHndlr 0xE6 CMD_AMI_GET_INVENTORY
// Privilege: User
//
// Request:  [Byte 0] paramSelector
//             0x00 = board/product info (default if absent)
//             0x04 = device status bitmap (32 bytes, all 0x00 = online)
//
// Response for param 0x00:
//   [0]     flags: 0x01 (data valid)
//   [1-32]  ProductName  (null-padded, 32 bytes)
//   [33-64] Manufacturer (null-padded, 32 bytes)
//   [65-96] SerialNumber (null-padded, 32 bytes)
//   [97-128] FirmwareVersion (null-padded, 32 bytes)
//
// Response for param 0x04:
//   [0-31]  DevStatus bitmap (all 0x00 = all devices online)
// -----------------------------------------------------------------------

static void copyField(std::vector<uint8_t>& out, const std::string& s,
                      size_t maxLen)
{
    for (size_t i = 0; i < maxLen; ++i)
        out.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0x00);
}

ipmi::RspType<std::vector<uint8_t>>
    ipmiGetInventory(ipmi::Context::ptr& /*ctx*/,
                     std::optional<uint8_t> paramSel)
{
    uint8_t param = paramSel.value_or(0x00);

    // Device status: all zeroes = all online
    if (param == 0x04)
    {
        std::vector<uint8_t> devStatus(32, 0x00);
        return ipmi::responseSuccess(devStatus);
    }

    // Board/product info block
    std::string productName  = "X570D4I-2T";
    std::string manufacturer = "ASRock Rack";
    std::string serialNumber;
    std::string fwVersion;

    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, itemBoardIntf, boardObjPath);

        auto tryProp = [&](const char* prop) -> std::string {
            try {
                ipmi::Value v = ipmi::getDbusProperty(
                    *dbus, svc, boardObjPath, assetIntf, prop);
                return std::get<std::string>(v);
            } catch (...) { return {}; }
        };

        if (auto n = tryProp("Model"); !n.empty())    productName  = n;
        if (auto m = tryProp("Manufacturer"); !m.empty()) manufacturer = m;
        if (auto s = tryProp("SerialNumber"); !s.empty()) serialNumber = s;
    }
    catch (...) {}

    // Active BMC firmware version from xyz.openbmc_project.Software objects
    try
    {
        auto dbus = getSdBus();
        using ObjTree = std::map<sdbusplus::message::object_path,
            std::map<std::string, std::map<std::string, ipmi::Value>>>;
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        msg.append(softwareRoot, 0,
                   std::vector<std::string>{softwareIntf, activationIntf});
        auto reply = dbus->call(msg);
        ObjTree objs;
        reply.read(objs);
        for (const auto& [path, ifaces] : objs)
        {
            if (!ifaces.count(activationIntf)) continue;
            auto it = ifaces.find(softwareIntf);
            if (it == ifaces.end()) continue;
            auto vIt = it->second.find("Version");
            if (vIt == it->second.end()) continue;
            auto ver = std::get<std::string>(vIt->second);
            if (!ver.empty()) { fwVersion = ver; break; }
        }
    }
    catch (...) {}

    std::vector<uint8_t> resp;
    resp.reserve(129);
    resp.push_back(0x01); // flags: data valid
    copyField(resp, productName,  32);
    copyField(resp, manufacturer, 32);
    copyField(resp, serialNumber, 32);
    copyField(resp, fwVersion,    32);
    return ipmi::responseSuccess(resp);
}

// -----------------------------------------------------------------------
// cmdGetSensorInfo (0x1E) — sensor name/type list from dbus-sensors
// Confirmed: g_AMI_CmdHndlr 0x1E, privilege 0x02 (User)
// Privilege: User
//
// Request:  [Byte 0] sensor index (0-based paging, 0xFF = all)
// Response: vector of { uint8_t nameLen, char name[nameLen] } records.
//           Returns at most 20 sensor names starting from the requested
//           index to keep the response within IPMI message limits.
// -----------------------------------------------------------------------

ipmi::RspType<std::vector<uint8_t>>
    ipmiGetSensorInfo(ipmi::Context::ptr& ctx, uint8_t startIndex)
{
    std::vector<std::string> sensorNames;

    try
    {
        auto dbus = getSdBus();
        // GetManagedObjects on the sensor namespace
        using ObjectValueTree = std::map<sdbusplus::message::object_path,
            std::map<std::string, std::map<std::string, ipmi::Value>>>;
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper",
            "GetSubTreePaths");
        msg.append(sensorRoot, 0, std::vector<std::string>{});
        auto reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);
        for (const auto& p : paths)
        {
            // Extract the last path component as the sensor name
            auto pos = p.rfind('/');
            if (pos != std::string::npos)
            {
                sensorNames.push_back(p.substr(pos + 1));
            }
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetSensorInfo: D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // Page: return up to 20 names starting at startIndex
    constexpr uint8_t pageSize = 20;
    std::vector<uint8_t> resp;

    if (startIndex >= sensorNames.size())
    {
        // Return count-only header indicating no entries at this offset
        resp.push_back(0); // count = 0
        return ipmi::responseSuccess(resp);
    }

    uint8_t count = 0;
    for (size_t i = startIndex;
         i < sensorNames.size() && count < pageSize; ++i, ++count)
    {
        const auto& name = sensorNames[i];
        uint8_t len = static_cast<uint8_t>(
            std::min(name.size(), static_cast<size_t>(0xFF)));
        resp.push_back(len);
        resp.insert(resp.end(), name.begin(), name.begin() + len);
    }

    // Prepend count
    resp.insert(resp.begin(), count);
    return ipmi::responseSuccess(resp);
}

// -----------------------------------------------------------------------
// cmdGetFwVersion (0x20) — BMC firmware version string
// Confirmed: g_AMI_CmdHndlr 0x20, privilege 0x10 (User)
// Privilege: User
//
// Response: { uint8_t major, uint8_t minor, uint8_t aux, char[N] versionStr }
// -----------------------------------------------------------------------

ipmi::RspType<std::vector<uint8_t>>
    ipmiGetFwVersion(ipmi::Context::ptr& /*ctx*/)
{
    std::string version = "unknown";

    try
    {
        auto dbus = getSdBus();
        // Walk /xyz/openbmc_project/software for the active BMC image
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper",
            "GetSubTreePaths");
        msg.append(softwareRoot, 0,
                   std::vector<std::string>{softwareIntf});
        auto reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        for (const auto& path : paths)
        {
            try
            {
                std::string svc = ipmi::getService(*dbus, softwareIntf, path);
                // Only return the BMC's own active image
                ipmi::Value actV = ipmi::getDbusProperty(
                    *dbus, svc, path, activationIntf, "Activation");
                const auto& actStr = std::get<std::string>(actV);
                if (actStr.find("Active") == std::string::npos)
                {
                    continue;
                }
                ipmi::Value verV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareIntf, "Version");
                version = std::get<std::string>(verV);
                break;
            }
            catch (...)
            {
                continue;
            }
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetFwVersion: D-Bus lookup failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // Pack as: major=0, minor=0, aux=0, then the version string with null
    std::vector<uint8_t> resp;
    resp.push_back(0); // major (parsed from string if needed)
    resp.push_back(0); // minor
    resp.push_back(0); // aux

    if (version.size() > 60)
    {
        version.resize(60);
    }
    version.push_back('\0');
    resp.insert(resp.end(), version.begin(), version.end());
    return ipmi::responseSuccess(resp);
}

// -----------------------------------------------------------------------
// cmdGetFwProtocol (0x21) — static OEM protocol version
// Confirmed: g_AMI_CmdHndlr 0x21, privilege 0x10 (User)
// Privilege: User
//
// Response: { uint8_t protocolVersion }
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t> ipmiGetFwProtocol(ipmi::Context::ptr& /*ctx*/)
{
    return ipmi::responseSuccess(oemProtocolVersion);
}

// -----------------------------------------------------------------------
// cmdMuxSwitching (0xEE) — KVM/SPI mux control via GPIOJ1 (line 73)
// Confirmed: g_AMI_CmdHndlr 0xEE CMD_AMI_MUX_SWITCHING
// Critical OEM command: bmc-analyze §Critical OEM Commands
// Privilege: User (privilege 0x01)
//
// The AST2500 GPIOJ1 (line 73) gates the SPI1 controller toward either
// the BMC or the host BIOS flash.  Setting it LOW muxes flash to BMC
// (needed for offline BIOS writes); setting it HIGH returns control to
// the host.
//
// Request:  [Byte 0] direction — 0=BMC (LOW, flash→BMC), 1=Host (HIGH)
// Response: [Byte 0] current GPIO value after the change
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t>
    ipmiMuxSwitching(ipmi::Context::ptr& /*ctx*/, uint8_t direction)
{
    if (direction > 1)
    {
        return ipmi::responseInvalidFieldRequest();
    }

    // Drive GPIOJ1 via the D-Bus GPIO interface exposed by phosphor-gpio-util
    // (or directly via the kernel /sys/class/gpio if not yet bridged).
    // For now use the D-Bus GpioInterface that entity-manager/phosphor-gpio
    // exposes at xyz.openbmc_project.Gpio.
    try
    {
        auto dbus = getSdBus();
        // The gpio-line-names entry for line 73 on the AST2500 is "SPI_MUX_SEL"
        // as defined in the X570D4I-2T DTS.  We call the phosphor GPIO manager.
        std::string svc = "xyz.openbmc_project.Gpio";
        std::string obj = "/xyz/openbmc_project/gpio/SPI_MUX_SEL";
        std::string intf = "xyz.openbmc_project.Gpio";

        auto setMsg = dbus->new_method_call(svc.c_str(), obj.c_str(),
                                             "org.freedesktop.DBus.Properties",
                                             "Set");
        setMsg.append(intf, "Value",
                      std::variant<bool>(direction != 0));
        dbus->call_noreply(setMsg);

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "ipmiMuxSwitching: GPIOJ1 set",
            phosphor::logging::entry("DIRECTION=%u", direction));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiMuxSwitching: GPIO control failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    return ipmi::responseSuccess(direction);
}

// -----------------------------------------------------------------------
// cmdPeciReadWrite (0xE9) — CPU thermal access via AMD APML/PECI
// Confirmed: g_AMI_CmdHndlr 0xE9 CMD_AMI_PECI_READ_WRITE
// Critical OEM command: bmc-analyze §Critical OEM Commands
// Privilege: User (privilege 0x03)
//
// On the X570D4I-2T the CPU thermal data flows through AMD APML (SBRMI)
// on I2C bus 1 (0x3C), not Intel PECI.  The SBRMI path is exposed by
// amd-apml kernel driver and surfaced via dbus-sensors' external-sensor.
// Until the APML bridge is fully wired, this handler returns the
// unspecified-error completion code so that the BIOS knows the command
// is present but not yet functional, rather than receiving the "invalid
// command" code that would indicate the feature is completely absent.
//
// Request:  [Byte 0] cpuAddr, [Byte 1] readLen, [Bytes 2..N] writeData
// Response: ipmi::responseUnspecifiedError() (not yet implemented)
// -----------------------------------------------------------------------

ipmi::RspType<> ipmiPeciReadWrite(ipmi::Context::ptr& /*ctx*/,
                                   uint8_t /*cpuAddr*/,
                                   uint8_t /*readLen*/,
                                   std::vector<uint8_t> /*writeData*/)
{
    // AMD APML bridge not yet implemented — return error rather than
    // ipmi::responseInvalidCommand() so the BIOS knows the slot exists.
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "ipmiPeciReadWrite: AMD APML not yet implemented");
    return ipmi::responseUnspecifiedError();
}

// -----------------------------------------------------------------------
// cmdPsuInfo (0xEC) — PSU present/online status aggregation
// Confirmed: g_AMI_CmdHndlr 0xEC CMD_AMI_PSU_INFO
// Critical OEM command: bmc-analyze §Critical OEM Commands
// Privilege: User (privilege 0xFF)
//
// The X570D4I-2T has one PSU monitored by phosphor-psu-monitor.
// We query xyz.openbmc_project.State.Decorator.OperationalStatus.Functional
// and return a one-byte summary:
//   bit 0 — PSU0 present (1=yes)
//   bit 1 — PSU0 online  (1=yes)
//
// Response: [Byte 0] PSU status bitmask
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t> ipmiPsuInfo(ipmi::Context::ptr& /*ctx*/)
{
    uint8_t status = 0;

    try
    {
        auto dbus = getSdBus();
        // Enumerate PSU objects
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper",
            "GetSubTreePaths");
        msg.append("/xyz/openbmc_project/inventory/system", 2,
                   std::vector<std::string>{psuItemIntf});
        auto reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        for (size_t i = 0; i < paths.size() && i < 4; ++i)
        {
            const auto& path = paths[i];
            try
            {
                // Present bit
                std::string svc = ipmi::getService(*dbus, psuItemIntf, path);
                ipmi::Value presentV = ipmi::getDbusProperty(
                    *dbus, svc, path,
                    "xyz.openbmc_project.Inventory.Item", "Present");
                bool present = std::get<bool>(presentV);
                if (present)
                {
                    status |= static_cast<uint8_t>(1 << (i * 2));
                }

                // Functional/online bit
                ipmi::Value funcV = ipmi::getDbusProperty(
                    *dbus, svc, path, operStateIntf, "Functional");
                bool functional = std::get<bool>(funcV);
                if (functional)
                {
                    status |= static_cast<uint8_t>(1 << (i * 2 + 1));
                }
            }
            catch (...)
            {
                continue;
            }
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiPsuInfo: D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    return ipmi::responseSuccess(status);
}

// -----------------------------------------------------------------------
// cmdManageBmcConfig (0x2B) — BMC warm/cold reset
// Confirmed: g_AMI_CmdHndlr 0x2B CMD_AMI_MANAGE_BMC_CONFIG, priv 0x01
// Privilege: User
//
// Request:  [Byte 0] action — 0x01=warm reset, 0x02=cold reset
// Response: completion code only
// -----------------------------------------------------------------------

ipmi::RspType<> ipmiManageBmcConfig(ipmi::Context::ptr& /*ctx*/,
                                     uint8_t action)
{
    if (action != 0x01 && action != 0x02)
    {
        return ipmi::responseInvalidFieldRequest();
    }

    try
    {
        auto dbus = getSdBus();
        std::string service = "xyz.openbmc_project.State.BMC";
        std::string objPath  = "/xyz/openbmc_project/state/bmc0";
        std::string intf     = "xyz.openbmc_project.State.BMC";

        const std::string targetState =
            (action == 0x02)
                ? "xyz.openbmc_project.State.BMC.Transition.HardReboot"
                : "xyz.openbmc_project.State.BMC.Transition.Reboot";

        ipmi::setDbusProperty(*dbus, service, objPath, intf,
                              "RequestedBMCTransition", targetState);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiManageBmcConfig: D-Bus reset request failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// cmdGetSelPolicy (0x30) — SEL wrap/linear policy
// Confirmed: g_AMI_CmdHndlr 0x30, priv 0x13 (Admin)
// Privilege: Admin
//
// phosphor-ipmi-host manages SEL wrap policy internally.  This handler
// returns a static response (circular/wrap = 0) so the BIOS can query
// the policy without receiving an "invalid command" error.
//
// Response: [Byte 0] policy (0=wrap, 1=linear/stop-when-full)
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t> ipmiGetSelPolicy(ipmi::Context::ptr& /*ctx*/)
{
    return ipmi::responseSuccess(static_cast<uint8_t>(0)); // wrap
}

// -----------------------------------------------------------------------
// YAFU stubs (0x01–0x10)
// Confirmed: g_AMI_CmdHndlr (bmc-analyze §MDR / YAFU table)
//
// The original AMI firmware used YAFU for BMC firmware upload over IPMI.
// On OpenBMC, phosphor-ipmi-blobs provides the equivalent mechanism.
// These stubs return ipmi::responseInvalidCommand() so that any BIOS or
// host utility probing for YAFU capability receives a defined response
// rather than an IPMI "destination unavailable" transport error.
// -----------------------------------------------------------------------

static ipmi::RspType<> ipmiYafuStub(ipmi::Context::ptr& /*ctx*/,
                                     [[maybe_unused]] std::vector<uint8_t> req)
{
    return ipmi::responseInvalidCommand();
}

// -----------------------------------------------------------------------
// Handler registration
// Runs before main() via __attribute__((constructor)).
// Follow the pattern from intel-ipmi-oem: one registerHandler() per command.
// -----------------------------------------------------------------------

static void registerOEMFunctions()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock OEM commands registered (NetFn 0x30)");

    // AMI inventory (0xE6) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetInventory),
                          ipmi::Privilege::User, ipmiGetInventory);

    // Sensor info (0x1E) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetSensorInfo),
                          ipmi::Privilege::User, ipmiGetSensorInfo);

    // Firmware version (0x20) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetFwVersion),
                          ipmi::Privilege::User, ipmiGetFwVersion);

    // Firmware protocol (0x21) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetFwProtocol),
                          ipmi::Privilege::User, ipmiGetFwProtocol);

    // BMC config management (0x2B) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdManageBmcConfig),
                          ipmi::Privilege::User, ipmiManageBmcConfig);

    // SEL policy get (0x30) — Admin
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetSelPolicy),
                          ipmi::Privilege::Admin, ipmiGetSelPolicy);

    // KVM mux switching (0xEE) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdMuxSwitching),
                          ipmi::Privilege::User, ipmiMuxSwitching);

    // PECI read/write (0xE9) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdPeciReadWrite),
                          ipmi::Privilege::User, ipmiPeciReadWrite);

    // PSU info (0xEC) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdPsuInfo),
                          ipmi::Privilege::User, ipmiPsuInfo);

    // YAFU stubs (0x01–0x10) — Admin; return invalidCommand
    for (uint8_t cmd = general::cmdYafuAllocateMemory;
         cmd <= general::cmdYafuEraseCopyFlash; ++cmd)
    {
        ipmi::registerHandler(ipmi::prioOemBase,
                              static_cast<ipmi::NetFn>(netFnGeneral),
                              static_cast<ipmi::Cmd>(cmd),
                              ipmi::Privilege::Admin, ipmiYafuStub);
    }
}

} // namespace asrock
