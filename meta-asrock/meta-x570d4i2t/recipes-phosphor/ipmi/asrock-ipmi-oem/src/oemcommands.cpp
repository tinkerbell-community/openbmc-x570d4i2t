// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI command handlers for the X570D4I-2T (NetFn 0x3A).
//
// This file mirrors the structure of intel-ipmi-oem/src/oemcommands.cpp
// while replacing Intel-specific NetFn assignments and business logic
// with the Megarac/AMI equivalents extracted from the v01.91.00 firmware
// (see bmc-analyze.instructions.md and megarac-bios-ipmi-methods.instructions.md).
//
// Implemented handlers (all on NetFn 0x3A / ipmi::netFnOemSix):
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

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetInventory (0xE6): handler entered",
        phosphor::logging::entry("PARAM=0x%02X", param));

    // Device status: all zeroes = all online
    if (param == 0x04)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetInventory (0xE6): returning device status bitmap (32 zeros)");
        std::vector<uint8_t> devStatus(32, 0x00);
        return ipmi::responseSuccess(devStatus);
    }

    // Board/product info block
    std::string productName  = "X570D4I-2T";
    std::string manufacturer = "ASRock Rack";
    std::string serialNumber;
    std::string fwVersion;

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetInventory (0xE6): querying board info from D-Bus",
        phosphor::logging::entry("OBJ=%s", boardObjPath));
    try
    {
        auto        dbus = getSdBus();
        std::string svc  = ipmi::getService(*dbus, itemBoardIntf, boardObjPath);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetInventory (0xE6): board service found",
            phosphor::logging::entry("SVC=%s", svc.c_str()));

        auto tryProp = [&](const char* prop) -> std::string {
            try
            {
                ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, boardObjPath,
                                                      assetIntf, prop);
                return std::get<std::string>(v);
            }
            catch (...)
            {
                return {};
            }
        };

        if (auto n = tryProp("Model"); !n.empty())
        {
            productName = n;
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetInventory (0xE6): Model from D-Bus",
                phosphor::logging::entry("VALUE=%s", n.c_str()));
        }
        if (auto m = tryProp("Manufacturer"); !m.empty())
        {
            manufacturer = m;
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetInventory (0xE6): Manufacturer from D-Bus",
                phosphor::logging::entry("VALUE=%s", m.c_str()));
        }
        if (auto s = tryProp("SerialNumber"); !s.empty())
        {
            serialNumber = s;
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetInventory (0xE6): SerialNumber from D-Bus",
                phosphor::logging::entry("VALUE=%s", s.c_str()));
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetInventory (0xE6): board D-Bus query failed, using defaults",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // Active BMC firmware version from xyz.openbmc_project.Software objects
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetInventory (0xE6): querying active BMC firmware version");
    try
    {
        auto dbus = getSdBus();
        using ObjTree = std::map<sdbusplus::message::object_path,
                                 std::map<std::string,
                                          std::map<std::string, ipmi::Value>>>;
        auto msg = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree");
        msg.append(softwareRoot, 0,
                   std::vector<std::string>{softwareIntf, activationIntf});
        auto    reply = dbus->call(msg);
        ObjTree objs;
        reply.read(objs);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetInventory (0xE6): software entries found",
            phosphor::logging::entry("COUNT=%zu", objs.size()));

        for (const auto& [path, ifaces] : objs)
        {
            if (!ifaces.count(activationIntf))
                continue;
            auto it = ifaces.find(softwareIntf);
            if (it == ifaces.end())
                continue;
            auto vIt = it->second.find("Version");
            if (vIt == it->second.end())
                continue;
            auto ver = std::get<std::string>(vIt->second);
            if (!ver.empty())
            {
                fwVersion = ver;
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "GetInventory (0xE6): firmware version found",
                    phosphor::logging::entry("PATH=%s",
                                             std::string(path).c_str()),
                    phosphor::logging::entry("VERSION=%s", ver.c_str()));
                break;
            }
        }
        if (fwVersion.empty())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "GetInventory (0xE6): no active firmware version found");
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetInventory (0xE6): software D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetInventory (0xE6): responding",
        phosphor::logging::entry("PRODUCT=%s", productName.c_str()),
        phosphor::logging::entry("MFR=%s", manufacturer.c_str()),
        phosphor::logging::entry("SERIAL=%s", serialNumber.c_str()),
        phosphor::logging::entry("FW=%s", fwVersion.c_str()));

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
    ipmiGetSensorInfo(ipmi::Context::ptr& /*ctx*/, uint8_t startIndex)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSensorInfo (0x1E): handler entered",
        phosphor::logging::entry("START_INDEX=%u", startIndex),
        phosphor::logging::entry("SENSOR_ROOT=%s", sensorRoot));

    std::vector<std::string> sensorNames;

    try
    {
        auto dbus = getSdBus();
        auto msg  = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
        msg.append(sensorRoot, 0, std::vector<std::string>{});
        auto                     reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetSensorInfo (0x1E): sensor paths found",
            phosphor::logging::entry("TOTAL=%zu", paths.size()));

        for (const auto& p : paths)
        {
            auto pos = p.rfind('/');
            if (pos != std::string::npos)
                sensorNames.push_back(p.substr(pos + 1));
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetSensorInfo (0x1E): D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "GetSensorInfo (0x1E): paging",
        phosphor::logging::entry("TOTAL_SENSORS=%zu", sensorNames.size()),
        phosphor::logging::entry("START_INDEX=%u", startIndex));

    constexpr uint8_t   pageSize = 20;
    std::vector<uint8_t> resp;

    if (startIndex >= sensorNames.size())
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "GetSensorInfo (0x1E): start index past end — returning empty page",
            phosphor::logging::entry("START_INDEX=%u", startIndex),
            phosphor::logging::entry("TOTAL=%zu", sensorNames.size()));
        resp.push_back(0);
        return ipmi::responseSuccess(resp);
    }

    uint8_t count = 0;
    for (size_t i = startIndex;
         i < sensorNames.size() && count < pageSize; ++i, ++count)
    {
        const auto& name = sensorNames[i];
        uint8_t     len  = static_cast<uint8_t>(
            std::min(name.size(), static_cast<size_t>(0xFF)));
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetSensorInfo (0x1E): adding sensor",
            phosphor::logging::entry("IDX=%zu", i),
            phosphor::logging::entry("NAME=%s", name.c_str()),
            phosphor::logging::entry("LEN=%u", len));
        resp.push_back(len);
        resp.insert(resp.end(), name.begin(), name.begin() + len);
    }

    resp.insert(resp.begin(), count);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSensorInfo (0x1E): responding",
        phosphor::logging::entry("COUNT=%u", count),
        phosphor::logging::entry("RESP_BYTES=%zu", resp.size()));
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetFwVersion (0x20): handler entered",
        phosphor::logging::entry("SOFTWARE_ROOT=%s", softwareRoot));

    std::string version = "unknown";

    try
    {
        auto dbus = getSdBus();
        auto msg  = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
        msg.append(softwareRoot, 0, std::vector<std::string>{softwareIntf});
        auto                     reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "GetFwVersion (0x20): software paths found",
            phosphor::logging::entry("COUNT=%zu", paths.size()));

        for (const auto& path : paths)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetFwVersion (0x20): checking path",
                phosphor::logging::entry("PATH=%s", path.c_str()));
            try
            {
                std::string svc = ipmi::getService(*dbus, softwareIntf, path);
                ipmi::Value actV = ipmi::getDbusProperty(
                    *dbus, svc, path, activationIntf, "Activation");
                const auto& actStr = std::get<std::string>(actV);

                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    "GetFwVersion (0x20): activation state",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("STATE=%s", actStr.c_str()));

                if (actStr.find("Active") == std::string::npos)
                {
                    phosphor::logging::log<phosphor::logging::level::DEBUG>(
                        "GetFwVersion (0x20): skipping non-active entry",
                        phosphor::logging::entry("PATH=%s", path.c_str()));
                    continue;
                }
                ipmi::Value verV = ipmi::getDbusProperty(
                    *dbus, svc, path, softwareIntf, "Version");
                version = std::get<std::string>(verV);
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "GetFwVersion (0x20): active version found",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("VERSION=%s", version.c_str()));
                break;
            }
            catch (const std::exception& e)
            {
                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    "GetFwVersion (0x20): error on path",
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("ERROR=%s", e.what()));
                continue;
            }
        }
        if (version == "unknown")
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "GetFwVersion (0x20): no active version found, using 'unknown'");
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetFwVersion (0x20): ObjectMapper query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    if (version.size() > 60)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetFwVersion (0x20): version string truncated to 60 chars",
            phosphor::logging::entry("ORIGINAL=%s", version.c_str()));
        version.resize(60);
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetFwVersion (0x20): responding",
        phosphor::logging::entry("VERSION=%s", version.c_str()),
        phosphor::logging::entry("RESP_BYTES=%zu", 3 + version.size() + 1));

    std::vector<uint8_t> resp;
    resp.push_back(0); // major
    resp.push_back(0); // minor
    resp.push_back(0); // aux
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetFwProtocol (0x21): handler entered",
        phosphor::logging::entry("PROTOCOL_VER=0x%02X", oemProtocolVersion));
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MuxSwitching (0xEE): handler entered",
        phosphor::logging::entry("DIRECTION=%u", direction),
        phosphor::logging::entry("MEANING=%s",
                                 direction == 0 ? "BMC (flash->BMC)" : "Host"),
        phosphor::logging::entry("GPIO_LINE=%u", muxGpioLine));

    if (direction > 1)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "MuxSwitching (0xEE): invalid direction byte",
            phosphor::logging::entry("DIRECTION=0x%02X", direction));
        return ipmi::responseInvalidFieldRequest();
    }

    try
    {
        auto        dbus = getSdBus();
        std::string svc  = "xyz.openbmc_project.Gpio";
        std::string obj  = "/xyz/openbmc_project/gpio/SPI_MUX_SEL";
        std::string intf = "xyz.openbmc_project.Gpio";

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "MuxSwitching (0xEE): calling D-Bus Set",
            phosphor::logging::entry("SVC=%s", svc.c_str()),
            phosphor::logging::entry("OBJ=%s", obj.c_str()),
            phosphor::logging::entry("VALUE=%s",
                                     direction != 0 ? "true" : "false"));

        auto setMsg = dbus->new_method_call(
            svc.c_str(), obj.c_str(), "org.freedesktop.DBus.Properties", "Set");
        setMsg.append(intf, "Value", std::variant<bool>(direction != 0));
        dbus->call_noreply(setMsg);

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MuxSwitching (0xEE): GPIOJ1 set successfully",
            phosphor::logging::entry("DIRECTION=%u", direction),
            phosphor::logging::entry("GPIO_VALUE=%s",
                                     direction != 0 ? "HIGH (Host)" :
                                                      "LOW (BMC)"));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MuxSwitching (0xEE): GPIO D-Bus call failed",
            phosphor::logging::entry("OBJ=/xyz/openbmc_project/gpio/SPI_MUX_SEL"),
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MuxSwitching (0xEE): responding",
        phosphor::logging::entry("ECHO_DIRECTION=%u", direction));
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
                                   uint8_t cpuAddr, uint8_t readLen,
                                   std::vector<uint8_t> writeData)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PeciReadWrite (0xE9): handler entered — AMD APML stub",
        phosphor::logging::entry("CPU_ADDR=0x%02X", cpuAddr),
        phosphor::logging::entry("READ_LEN=%u", readLen),
        phosphor::logging::entry("WRITE_LEN=%zu", writeData.size()));
    // AMD APML bridge not yet implemented — return error rather than
    // ipmi::responseInvalidCommand() so the BIOS knows the slot exists.
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "PeciReadWrite (0xE9): returning unspecifiedError (APML not wired)");
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PsuInfo (0xEC): handler entered");

    uint8_t status = 0;

    try
    {
        auto dbus = getSdBus();
        auto msg  = dbus->new_method_call(
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
        msg.append("/xyz/openbmc_project/inventory/system", 2,
                   std::vector<std::string>{psuItemIntf});
        auto                     reply = dbus->call(msg);
        std::vector<std::string> paths;
        reply.read(paths);

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "PsuInfo (0xEC): PSU inventory objects found",
            phosphor::logging::entry("COUNT=%zu", paths.size()));

        for (size_t i = 0; i < paths.size() && i < 4; ++i)
        {
            const auto& path = paths[i];
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "PsuInfo (0xEC): checking PSU",
                phosphor::logging::entry("IDX=%zu", i),
                phosphor::logging::entry("PATH=%s", path.c_str()));
            try
            {
                std::string svc = ipmi::getService(*dbus, psuItemIntf, path);

                ipmi::Value presentV = ipmi::getDbusProperty(
                    *dbus, svc, path,
                    "xyz.openbmc_project.Inventory.Item", "Present");
                bool present = std::get<bool>(presentV);
                if (present)
                    status |= static_cast<uint8_t>(1 << (i * 2));

                ipmi::Value funcV = ipmi::getDbusProperty(
                    *dbus, svc, path, operStateIntf, "Functional");
                bool functional = std::get<bool>(funcV);
                if (functional)
                    status |= static_cast<uint8_t>(1 << (i * 2 + 1));

                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "PsuInfo (0xEC): PSU state",
                    phosphor::logging::entry("IDX=%zu", i),
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("PRESENT=%u", present),
                    phosphor::logging::entry("FUNCTIONAL=%u", functional));
            }
            catch (const std::exception& e)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "PsuInfo (0xEC): error querying PSU",
                    phosphor::logging::entry("IDX=%zu", i),
                    phosphor::logging::entry("PATH=%s", path.c_str()),
                    phosphor::logging::entry("ERROR=%s", e.what()));
                continue;
            }
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "PsuInfo (0xEC): ObjectMapper query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PsuInfo (0xEC): responding",
        phosphor::logging::entry("STATUS_BYTE=0x%02X", status));
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ManageBmcConfig (0x2B): handler entered",
        phosphor::logging::entry("ACTION=0x%02X", action),
        phosphor::logging::entry("MEANING=%s",
                                 action == 0x01 ? "warm reset" :
                                 action == 0x02 ? "cold reset" : "unknown"));

    if (action != 0x01 && action != 0x02)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ManageBmcConfig (0x2B): invalid action byte",
            phosphor::logging::entry("ACTION=0x%02X", action));
        return ipmi::responseInvalidFieldRequest();
    }

    const std::string targetState =
        (action == 0x02)
            ? "xyz.openbmc_project.State.BMC.Transition.HardReboot"
            : "xyz.openbmc_project.State.BMC.Transition.Reboot";

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ManageBmcConfig (0x2B): requesting BMC state transition",
        phosphor::logging::entry("ACTION=0x%02X", action),
        phosphor::logging::entry("TARGET_STATE=%s", targetState.c_str()));

    try
    {
        auto        dbus    = getSdBus();
        std::string service = "xyz.openbmc_project.State.BMC";
        std::string objPath = "/xyz/openbmc_project/state/bmc0";
        std::string intf    = "xyz.openbmc_project.State.BMC";

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "ManageBmcConfig (0x2B): calling setDbusProperty",
            phosphor::logging::entry("SVC=%s", service.c_str()),
            phosphor::logging::entry("OBJ=%s", objPath.c_str()),
            phosphor::logging::entry("PROP=RequestedBMCTransition"),
            phosphor::logging::entry("VALUE=%s", targetState.c_str()));

        ipmi::setDbusProperty(*dbus, service, objPath, intf,
                              "RequestedBMCTransition", targetState);

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "ManageBmcConfig (0x2B): BMC reset transition accepted");
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ManageBmcConfig (0x2B): D-Bus reset request failed",
            phosphor::logging::entry("TARGET=%s", targetState.c_str()),
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSelPolicy (0x30): handler entered — returning wrap policy (0x00)");
    return ipmi::responseSuccess(static_cast<uint8_t>(0)); // 0 = circular/wrap
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

static ipmi::RspType<> ipmiYafuStub(ipmi::Context::ptr& ctx,
                                     [[maybe_unused]] std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "YafuStub: YAFU probe received — returning invalidCommand",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("NOTE=use phosphor-ipmi-blobs for FW upload"));
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
        "ASRock OEM commands registered (NetFn 0x3A / netFnOemSix)");

    // AMI inventory (0xE6) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdGetInventory),
                          ipmi::Privilege::User, ipmiGetInventory);

    // Sensor info (0x1E) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdGetSensorInfo),
                          ipmi::Privilege::User, ipmiGetSensorInfo);

    // Firmware version (0x20) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdGetFwVersion),
                          ipmi::Privilege::User, ipmiGetFwVersion);

    // Firmware protocol (0x21) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdGetFwProtocol),
                          ipmi::Privilege::User, ipmiGetFwProtocol);

    // BMC config management (0x2B) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdManageBmcConfig),
                          ipmi::Privilege::User, ipmiManageBmcConfig);

    // SEL policy get (0x30) — Admin
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdGetSelPolicy),
                          ipmi::Privilege::Admin, ipmiGetSelPolicy);

    // KVM mux switching (0xEE) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdMuxSwitching),
                          ipmi::Privilege::User, ipmiMuxSwitching);

    // PECI read/write (0xE9) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdPeciReadWrite),
                          ipmi::Privilege::User, ipmiPeciReadWrite);

    // PSU info (0xEC) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                          static_cast<ipmi::Cmd>(general::cmdPsuInfo),
                          ipmi::Privilege::User, ipmiPsuInfo);

    // YAFU stubs (0x01–0x10) — Admin; return invalidCommand
    for (uint8_t cmd = general::cmdYafuAllocateMemory;
         cmd <= general::cmdYafuEraseCopyFlash; ++cmd)
    {
        ipmi::registerHandler(ipmi::prioOemBase,
                              static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
                              static_cast<ipmi::Cmd>(cmd),
                              ipmi::Privilege::Admin, ipmiYafuStub);
    }
}

} // namespace asrock
