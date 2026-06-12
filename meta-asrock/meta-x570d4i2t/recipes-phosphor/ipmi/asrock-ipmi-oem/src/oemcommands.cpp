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
#include <amiconverter.hpp>
#include <smbiosbuilder.hpp>

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
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

// ═══════════════════════════════════════════════════════════════════════
// AMI MDR SMBIOS hooks
//
// These handlers are registered at prioOemBase (> prioOpenBmcBase used by
// smbiosmdrv2handler.cpp) so they WIN on every shared command code and log
// the exact byte payloads the BIOS sends. That gives full protocol visibility
// in journalctl during POST without a separate bus trace.
//
// Order of operations mirrors ami-ipmi-oem.cpp (the reference guide):
//   Write path: 0x3D GetMdrStatus → 0x51 WriteBegin → 0x52 WriteChunk(s)
//               → 0x53 WriteEnd; or legacy 0x5D phase=01/02
//   Read  path: 0x71 RegionStatus → 0x72 GetBlock (chunked reads)
//   Agent/Dir:  0x31 GetDir
//   AMI propr:  0xB2 SetBiosInfo · 0xA0 SetMdrPos · 0xB5 SetSmbiosChunk
//               · 0xA1 GetMdrStatus · 0xF3 GetStatus
//
// NOTE: 0x30 (AgentStatus) intentionally NOT added — it collides with the
// existing GetSelPolicy handler on the same NetFn/Cmd slot.
// ═══════════════════════════════════════════════════════════════════════

// ── Command codes (AMI MDR on NetFn 0x3A) ──────────────────────────────
static constexpr ipmi::Cmd kMdrGetDir        = 0x31;
static constexpr ipmi::Cmd kMdrGetStatus     = 0x3D;
static constexpr ipmi::Cmd kMdrWriteBegin    = 0x51;
static constexpr ipmi::Cmd kMdrWriteChunk    = 0x52;
static constexpr ipmi::Cmd kMdrWriteEnd      = 0x53;
static constexpr ipmi::Cmd kMdrLegacyCtrl    = 0x5D;
static constexpr ipmi::Cmd kMdrRegionStatus  = 0x71;
static constexpr ipmi::Cmd kMdrGetBlock      = 0x72;
static constexpr ipmi::Cmd kAmiSetMdrPos     = 0xA0;
static constexpr ipmi::Cmd kAmiGetMdrStatus  = 0xA1;
static constexpr ipmi::Cmd kAmiSetBiosInfo   = 0xB2;
static constexpr ipmi::Cmd kAmiSetSmbiosChunk = 0xB5;
static constexpr ipmi::Cmd kAmiGetStatus     = 0xF3;

// ── Region IDs ──────────────────────────────────────────────────────────
static constexpr uint8_t kOemRegionSmbios = 0;
static constexpr uint8_t kOemRegionMeta   = 1;

// ── Write-path accumulation state ───────────────────────────────────────
enum class OemMdrState : uint8_t { Idle, Open, Receiving };
static OemMdrState          g_oemState       = OemMdrState::Idle;
static uint32_t             g_oemDeclared    = 0;
static std::vector<uint8_t> g_oemWriteBuf;
// True only after the BIOS completes a WriteEnd with real SMBIOS data.
// RegionStatus for region 0 returns valid=0 until this is set so the BIOS
// always pushes a fresh SMBIOS table (not just meta updates).
static bool                 g_hasBiosPushedSmbios = false;

// ── Hex-dump helper (logs up to 64 bytes) ───────────────────────────────
static std::string mdrHex(const std::vector<uint8_t>& v)
{
    std::ostringstream os;
    size_t limit = std::min(v.size(), size_t{64});
    for (size_t i = 0; i < limit; ++i)
    {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", v[i]);
        os << b;
    }
    if (v.size() > limit) os << "...(" << v.size() << "B)";
    return os.str();
}

// ── Catch-all probe handler ──────────────────────────────────────────────
// Registered at prioOpenBmcBase so real handlers at prioOemBase always win.
// Returns CC=0 + 1-byte success — error responses cause BIOS to abort MDR.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiProbeCmdHandler(ipmi::Context::ptr& ctx, std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PROBE unhandled OEM cmd",
        phosphor::logging::entry("NETFN=0x%02X", static_cast<unsigned>(ctx->netFn)),
        phosphor::logging::entry("CMD=0x%02X",   static_cast<unsigned>(ctx->cmd)),
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s",   mdrHex(req).c_str()));
    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// ── 0x31 GetDir ─────────────────────────────────────────────────────────
// Caller: BIOS during POST. Request = [agentId:2 LE, dirIndex:1].
// Returns 1 SMBIOS region entry with valid=0 (BMC ready to receive).
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrGetDir(ipmi::Context::ptr& /*ctx*/,
                  std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x31 GetDir",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    uint16_t agentId  = (req.size() >= 2)
                            ? static_cast<uint16_t>(req[0] | (req[1] << 8))
                            : 0;
    uint8_t  dirIndex = (req.size() >= 3) ? req[2] : 0;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x31 GetDir parsed",
        phosphor::logging::entry("AGENT_ID=0x%04X", agentId),
        phosphor::logging::entry("DIR_INDEX=%u", dirIndex));

    // Response: [dirVersion:1, entryCount:1, remaining:1, entry0(16), entry1(16)]
    // Each 16-byte entry: [regionId, valid, sizeLo, sizeHi, usedLo, usedHi,
    //                      maxLo, maxHi, checksum, pad×7]
    // Entry 0 = SMBIOS table (valid=0 → BMC ready to receive, BIOS will push)
    // Entry 1 = Anchor/meta  (valid=1 → 31-byte fixed anchor, always present)
    // NOTE: Both entries MUST be present or the BIOS aborts the MDR sequence.
    auto writeEntry = [](uint8_t* out, uint8_t regionId, bool valid,
                         uint16_t size, uint16_t maxSize, uint8_t checksum) {
        std::memset(out, 0, 16);
        out[0] = regionId;
        out[1] = valid ? 0x01u : 0x00u;
        out[2] = size & 0xFF;          // total size  LSB
        out[3] = (size >> 8) & 0xFF;   // total size  MSB
        out[4] = size & 0xFF;          // used  size  LSB (== total when valid)
        out[5] = (size >> 8) & 0xFF;   // used  size  MSB
        out[6] = maxSize & 0xFF;       // max   size  LSB
        out[7] = (maxSize >> 8) & 0xFF;// max   size  MSB
        out[8] = checksum;
    };

    std::vector<uint8_t> rsp(3 + 16 + 16, 0);
    rsp[0] = 0x01;  // dirVersion
    rsp[1] = 0x02;  // entryCount = 2 (SMBIOS table + anchor)
    rsp[2] = 0x00;  // remaining
    writeEntry(rsp.data() + 3,      kOemRegionSmbios,
               /*valid*/ false, /*size*/ 0, /*maxSize*/ 0xFFFF, /*chk*/ 0);
    writeEntry(rsp.data() + 3 + 16, kOemRegionMeta,
               /*valid*/ true,  /*size*/ 31, /*maxSize*/ 31, /*chk*/ 0);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x31 GetDir RSP 2-entry (e0=invalid/ready, e1=anchor/valid)",
        phosphor::logging::entry("RSP_HEX=%s", mdrHex(rsp).c_str()));
    return ipmi::responseSuccess(rsp);
}

// ── 0x3D GetMdrStatus ───────────────────────────────────────────────────
// BIOS handshake: advertise max-chunk and version.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrGetStatus(ipmi::Context::ptr& /*ctx*/,
                     std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x3D GetMdrStatus",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()),
        phosphor::logging::entry("STATE=%u", static_cast<uint8_t>(g_oemState)),
        phosphor::logging::entry("BUF_BYTES=%zu", g_oemWriteBuf.size()));

    // [status, maxChunkLo, maxChunkHi, mdrVer]
    // status 0x01 = "need data" until BIOS completes WriteEnd with real table
    // status 0x00 = "has data" after BIOS has successfully pushed SMBIOS
    uint8_t status = g_hasBiosPushedSmbios ? 0x00 : 0x01;
    std::vector<uint8_t> rsp = {status, 0x00, 0x10, 0x01};

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x3D GetMdrStatus RSP",
        phosphor::logging::entry("STATUS=0x%02X", status),
        phosphor::logging::entry("RSP_HEX=%s", mdrHex(rsp).c_str()));
    return ipmi::responseSuccess(rsp);
}

// ── 0x51 WriteBegin ──────────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrWriteBegin(ipmi::Context::ptr& /*ctx*/,
                      std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x51 WriteBegin",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    uint32_t declared = 0;
    if (req.size() >= 3)
        declared = static_cast<uint32_t>(req[1]) | (static_cast<uint32_t>(req[2]) << 8);
    else if (req.size() >= 2)
        declared = req[1];

    if (declared > ami::kMaxPayload)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "MDR 0x51 WriteBegin: declared > kMaxPayload, capping",
            phosphor::logging::entry("DECLARED=%u", declared),
            phosphor::logging::entry("CAP=%u", ami::kMaxPayload));
        declared = ami::kMaxPayload;
    }
    g_oemWriteBuf.clear();
    if (declared) g_oemWriteBuf.reserve(declared);
    g_oemDeclared = declared;
    g_oemState    = OemMdrState::Open;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x51 WriteBegin: session opened",
        phosphor::logging::entry("DECLARED_BYTES=%u", declared));

    return ipmi::responseSuccess(std::vector<uint8_t>{});
}

// ── 0x52 WriteChunk ──────────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrWriteChunk(ipmi::Context::ptr& /*ctx*/,
                      std::vector<uint8_t> req)
{
    if (req.size() < 4)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "MDR 0x52 WriteChunk: too short",
            phosphor::logging::entry("REQ_BYTES=%zu", req.size()));
        return ipmi::responseReqDataLenInvalid();
    }

    uint8_t  regionId  = req[0];
    uint16_t offset    = static_cast<uint16_t>(req[1]) | (static_cast<uint16_t>(req[2]) << 8);
    size_t   payloadSz = req.size() - 3;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x52 WriteChunk",
        phosphor::logging::entry("REGION=%u", regionId),
        phosphor::logging::entry("OFFSET=0x%04X", offset),
        phosphor::logging::entry("PAYLOAD_BYTES=%zu", payloadSz),
        phosphor::logging::entry("FIRST_HEX=%s",
            mdrHex(std::vector<uint8_t>(req.begin()+3,
                   req.begin()+3+std::min(payloadSz,size_t{16}))).c_str()));

    if (g_oemState == OemMdrState::Idle)
    {
        g_oemWriteBuf.clear();
        g_oemDeclared = 0;
        g_oemState    = OemMdrState::Open;
    }

    size_t needed = static_cast<size_t>(offset) + payloadSz;
    if (needed > ami::kMaxPayload)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MDR 0x52 WriteChunk: oversized, rejecting",
            phosphor::logging::entry("OFFSET=0x%04X", offset),
            phosphor::logging::entry("PAYLOAD=%zu", payloadSz),
            phosphor::logging::entry("NEEDED=%zu", needed));
        return ipmi::responseReqDataLenInvalid();
    }
    if (needed > g_oemWriteBuf.size())
        g_oemWriteBuf.resize(needed, 0);
    std::copy(req.begin() + 3, req.end(), g_oemWriteBuf.begin() + offset);
    g_oemState = OemMdrState::Receiving;

    return ipmi::responseSuccess(std::vector<uint8_t>{});
}

// ── 0x53 WriteEnd ────────────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrWriteEnd(ipmi::Context::ptr& /*ctx*/,
                    std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x53 WriteEnd",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()),
        phosphor::logging::entry("ACCUM_BYTES=%zu", g_oemWriteBuf.size()),
        phosphor::logging::entry("DECLARED=%u", g_oemDeclared));

    if (!g_oemWriteBuf.empty())
    {
        // Log first 32 bytes of the accumulated SMBIOS payload
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MDR 0x53 WriteEnd: SMBIOS payload head",
            phosphor::logging::entry("HEAD_HEX=%s", mdrHex(g_oemWriteBuf).c_str()));

        if (ami::persistAmiBuffer(g_oemWriteBuf.data(), g_oemWriteBuf.size()))
        {
            ami::triggerMdrSync();
            g_hasBiosPushedSmbios = true;  // BIOS has successfully pushed SMBIOS
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "MDR 0x53 WriteEnd: committed to disk and synced",
                phosphor::logging::entry("BYTES=%zu", g_oemWriteBuf.size()));
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "MDR 0x53 WriteEnd: persistAmiBuffer failed");
        }
        g_oemWriteBuf.clear();
    }
    g_oemState    = OemMdrState::Idle;
    g_oemDeclared = 0;

    return ipmi::responseSuccess(std::vector<uint8_t>{});
}

// Synthesize the SMBIOS table (host fields + FRU + SPD + static) and persist
// it as MDR V2 for smbios-mdr + the BIOS's 0x72 GetBlock read-back.
static bool rebuildAndPersistSmbios()
{
    std::vector<uint8_t> table = smbiosbuild::buildSmbiosTable();
    if (table.empty())
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "rebuildAndPersistSmbios: empty table, skipping");
        return false;
    }
    if (!ami::writeMdrFile(table.data(), table.size()))
    {
        return false;
    }
    ami::triggerMdrSync();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "rebuildAndPersistSmbios: SMBIOS synthesized + synced",
        phosphor::logging::entry("BYTES=%zu", table.size()));
    return true;
}

// ── 0x5D LegacyCtrl (two-phase begin/end) ───────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrLegacyCtrl(ipmi::Context::ptr& /*ctx*/,
                      std::vector<uint8_t> req)
{
    uint8_t phase = req.empty() ? 0 : req[0];
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x5D LegacyCtrl",
        phosphor::logging::entry("PHASE=0x%02X", phase),
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()),
        phosphor::logging::entry("BUF_BYTES=%zu", g_oemWriteBuf.size()));

    if (phase == 0x01)
    {
        g_oemWriteBuf.clear();
        g_oemDeclared = 0;
        g_oemState    = OemMdrState::Open;
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MDR 0x5D phase=01: session opened");
    }
    else if (phase == 0x02)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MDR 0x5D phase=02: commit",
            phosphor::logging::entry("ACCUM_BYTES=%zu", g_oemWriteBuf.size()));
        if (!g_oemWriteBuf.empty())
        {
            // Host actually streamed a full table (not seen on this BIOS, but
            // honor it if it ever does): persist the raw buffer verbatim.
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "MDR 0x5D payload head",
                phosphor::logging::entry("HEAD_HEX=%s", mdrHex(g_oemWriteBuf).c_str()));
            if (ami::persistAmiBuffer(g_oemWriteBuf.data(), g_oemWriteBuf.size()))
            {
                ami::triggerMdrSync();
                g_hasBiosPushedSmbios = true;
            }
            g_oemWriteBuf.clear();
        }
        else
        {
            // Normal path: the BIOS only pushed OEM fields (0xB2 version,
            // 0xB5 board name) and now commits. Synthesize the full table
            // from those fields + FRU + DDR4 SPD + static board constants.
            if (rebuildAndPersistSmbios())
            {
                g_hasBiosPushedSmbios = true;
            }
        }
        g_oemState    = OemMdrState::Idle;
        g_oemDeclared = 0;
    }

    // AMI BIOS expects 1-byte status; without it the KCS driver waits ~5s.
    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// ── 0x71 RegionStatus ───────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrRegionStatus(ipmi::Context::ptr& /*ctx*/,
                        std::vector<uint8_t> req)
{
    uint8_t regionId = req.empty() ? 0 : req[0];
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x71 RegionStatus",
        phosphor::logging::entry("REGION=%u", regionId),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()),
        phosphor::logging::entry("STATE=%u", static_cast<uint8_t>(g_oemState)));

    auto cached = ami::loadMdrPayload();
    // Region 0 (SMBIOS table): only valid after BIOS completes a WriteEnd.
    // FRU-derived data doesn't count — returning valid=0 forces the BIOS
    // to push the full SMBIOS table rather than only sending meta updates.
    // Region 1 (Anchor): always valid (synthesized 31-byte anchor).
    bool valid = (regionId == kOemRegionSmbios)
                     ? g_hasBiosPushedSmbios
                     : true;
    uint16_t sz  = (valid && !cached.empty()) ? static_cast<uint16_t>(cached.size()) : 0;
    uint16_t chk = (valid && !cached.empty()) ? ami::computeChecksum(cached.data(), cached.size()) : 0;

    // [mdrVer, regionId, valid, lock, updateCnt, szLo, szHi, usedLo, usedHi, chkLo]
    std::vector<uint8_t> rsp(10, 0);
    rsp[0] = 0x01;
    rsp[1] = regionId;
    rsp[2] = valid ? 0x01 : 0x00;
    rsp[3] = (g_oemState != OemMdrState::Idle) ? 0x01 : 0x00;
    rsp[4] = 0x00;
    rsp[5] = sz  & 0xFF;
    rsp[6] = (sz  >> 8) & 0xFF;
    rsp[7] = sz  & 0xFF;
    rsp[8] = (sz  >> 8) & 0xFF;
    rsp[9] = chk & 0xFF;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x71 RegionStatus RSP",
        phosphor::logging::entry("VALID=%u", valid),
        phosphor::logging::entry("SIZE=%u", sz),
        phosphor::logging::entry("RSP_HEX=%s", mdrHex(rsp).c_str()));
    return ipmi::responseSuccess(rsp);
}

// ── 0x72 GetBlock ────────────────────────────────────────────────────────
// BIOS reads SMBIOS data. Region 0 = payload; Region 1 = meta header.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiMdrGetBlock(ipmi::Context::ptr& /*ctx*/,
                    std::vector<uint8_t> req)
{
    if (req.size() < 3)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "MDR 0x72 GetBlock: too short",
            phosphor::logging::entry("REQ_BYTES=%zu", req.size()));
        return ipmi::responseReqDataLenInvalid();
    }
    uint8_t  regionId = req[0];
    uint16_t offset   = static_cast<uint16_t>(req[1]) | (static_cast<uint16_t>(req[2]) << 8);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x72 GetBlock",
        phosphor::logging::entry("REGION=%u", regionId),
        phosphor::logging::entry("OFFSET=0x%04X", offset),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    auto cached = ami::loadMdrPayload();

    std::vector<uint8_t> source;
    if (regionId == kOemRegionMeta)
    {
        // Synthesize a 4-byte mini meta-header [dataSize:2LE, checksum:2LE]
        uint16_t sz  = static_cast<uint16_t>(cached.size());
        uint16_t chk = ami::computeChecksum(cached.data(), cached.size());
        source = {static_cast<uint8_t>(sz & 0xFF),
                  static_cast<uint8_t>((sz >> 8) & 0xFF),
                  static_cast<uint8_t>(chk & 0xFF),
                  static_cast<uint8_t>((chk >> 8) & 0xFF)};
    }
    else if (regionId == kOemRegionSmbios)
    {
        source = cached;
    }

    if (source.empty() || offset >= source.size())
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MDR 0x72 GetBlock: empty/past-end",
            phosphor::logging::entry("REGION=%u", regionId),
            phosphor::logging::entry("SOURCE_SIZE=%zu", source.size()));
        return ipmi::responseSuccess(std::vector<uint8_t>{});
    }

    constexpr size_t kChunk = 4096;
    size_t sendLen = std::min(source.size() - offset, kChunk);
    std::vector<uint8_t> rsp(source.begin() + offset,
                              source.begin() + offset + sendLen);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0x72 GetBlock RSP",
        phosphor::logging::entry("REGION=%u", regionId),
        phosphor::logging::entry("SEND_BYTES=%zu", sendLen),
        phosphor::logging::entry("TOTAL=%zu", source.size()),
        phosphor::logging::entry("HEAD_HEX=%s", mdrHex(rsp).c_str()));
    return ipmi::responseSuccess(rsp);
}

// ── 0xA0 SetMdrPos / BackupBmcMacDxe ────────────────────────────────────
// BIOS RE shows this is BackupBmcMacDxe: body = [LAN_channel:1][MAC:6]
static ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiSetMdrPos(ipmi::Context::ptr& /*ctx*/,
                     std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI 0xA0 SetMdrPos",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    if (req.size() >= 7)
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "AMI 0xA0 SetMdrPos: BackupBmcMacDxe",
            phosphor::logging::entry("LAN_CH=%u", req[0]),
            phosphor::logging::entry("MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                req[1], req[2], req[3], req[4], req[5], req[6]));

    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// ── 0xA1 GetMdrStatus ───────────────────────────────────────────────────
// BIOS queries current region size + checksum.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiGetMdrStatus(ipmi::Context::ptr& /*ctx*/,
                        std::vector<uint8_t> req)
{
    uint8_t regionId = req.empty() ? 0 : req[0];
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI 0xA1 GetMdrStatus",
        phosphor::logging::entry("REGION=%u", regionId),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    auto cached = ami::loadMdrPayload();
    bool     valid = !cached.empty();
    uint16_t sz    = valid ? static_cast<uint16_t>(cached.size()) : 0;
    uint16_t chk   = valid ? ami::computeChecksum(cached.data(), cached.size()) : 0;

    std::vector<uint8_t> rsp = {
        regionId,
        valid ? uint8_t{0x01} : uint8_t{0x00},
        static_cast<uint8_t>(sz  & 0xFF),
        static_cast<uint8_t>((sz  >> 8) & 0xFF),
        static_cast<uint8_t>(chk & 0xFF),
        static_cast<uint8_t>((chk >> 8) & 0xFF),
    };
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR 0xA1 GetMdrStatus RSP",
        phosphor::logging::entry("VALID=%u", valid),
        phosphor::logging::entry("SIZE=%u", sz),
        phosphor::logging::entry("RSP_HEX=%s", mdrHex(rsp).c_str()));
    return ipmi::responseSuccess(rsp);
}

// ── 0xB2 SetBiosInfo ────────────────────────────────────────────────────
// BIOS sends a 16-byte version block. Log every byte for decoding.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiSetBiosInfo(ipmi::Context::ptr& /*ctx*/,
                       std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI 0xB2 SetBiosInfo",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    if (req.size() >= 8)
    {
        uint8_t major   = req[1];
        uint8_t minorHi = (req[2] >> 4) & 0xF;
        uint8_t minorLo =  req[2]       & 0xF;
        uint8_t revCh   = req[3];
        char ver[16];
        if (revCh >= 0x20 && revCh < 0x7F && revCh != ' ')
            snprintf(ver, sizeof(ver), "%u.%u%u%c", major, minorHi, minorLo, revCh);
        else
            snprintf(ver, sizeof(ver), "%u.%u%u",   major, minorHi, minorLo);

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "AMI 0xB2 SetBiosInfo decoded",
            phosphor::logging::entry("VERSION=%s", ver),
            phosphor::logging::entry("DATE_RAW=%02X %02X %02X %02X",
                req[4], req[5], req[6], req[7]));

        // Capture version for SMBIOS Type 0. The 0xB2 date-field encoding is
        // not yet decoded, so leave the date to the builder's neutral fallback.
        smbiosbuild::setHostBios(ver, "");
    }
    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// ── 0xB5 SetSmbiosChunk ──────────────────────────────────────────────────
// Wire format: [reserved=0x00][ASCII string...][NUL].
// Each call is one SMBIOS string fragment pushed by SendInfoBmcIpmiDxe.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiSetSmbiosChunk(ipmi::Context::ptr& /*ctx*/,
                          std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI 0xB5 SetSmbiosChunk",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()));

    // Extract the null-terminated string after the reserved byte
    if (req.size() >= 2 && req[0] == 0x00)
    {
        size_t end = req.size();
        if (end > 1 && req[end - 1] == 0x00) end--;
        std::string s(req.begin() + 1, req.begin() + end);
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "AMI 0xB5 SetSmbiosChunk string",
            phosphor::logging::entry("VALUE=%s", s.c_str()),
            phosphor::logging::entry("LEN=%zu", s.size()));

        // Capture the board/product name for SMBIOS Type 1/2 synthesis.
        smbiosbuild::setHostBoardName(s);
    }
    return ipmi::responseSuccess(std::vector<uint8_t>{0x01});
}

// ── 0xF3 GetStatus ───────────────────────────────────────────────────────
// BIOS polls until BMC acks completion. We always ack ready.
static ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiGetStatus(ipmi::Context::ptr& /*ctx*/,
                     std::vector<uint8_t> req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI 0xF3 GetStatus",
        phosphor::logging::entry("REQ_BYTES=%zu", req.size()),
        phosphor::logging::entry("REQ_HEX=%s", mdrHex(req).c_str()),
        phosphor::logging::entry("STATE=%u", static_cast<uint8_t>(g_oemState)),
        phosphor::logging::entry("BUF_BYTES=%zu", g_oemWriteBuf.size()));
    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// ═══════════════════════════════════════════════════════════════════════
// end MDR SMBIOS hooks
// ═══════════════════════════════════════════════════════════════════════

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

    // ── MDR SMBIOS hooks — registered on BOTH NetFn 0x32 and NetFn 0x3A.
    //    The AMI BIOS uses both NetFns interchangeably for MDR; missing 0x32
    //    causes CC errors that make the BIOS abort its MDR SMM handler.
    for (auto mdrNetFn : {static_cast<ipmi::NetFn>(ipmi::netFnOemTwo),
                          static_cast<ipmi::NetFn>(ipmi::netFnOemSix)})
    {
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrGetDir,       ipmi::Privilege::Admin, ipmiMdrGetDir);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrGetStatus,    ipmi::Privilege::Admin, ipmiMdrGetStatus);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrWriteBegin,   ipmi::Privilege::Admin, ipmiMdrWriteBegin);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrWriteChunk,   ipmi::Privilege::Admin, ipmiMdrWriteChunk);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrWriteEnd,     ipmi::Privilege::Admin, ipmiMdrWriteEnd);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrLegacyCtrl,   ipmi::Privilege::Admin, ipmiMdrLegacyCtrl);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrRegionStatus, ipmi::Privilege::Admin, ipmiMdrRegionStatus);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kMdrGetBlock,     ipmi::Privilege::Admin, ipmiMdrGetBlock);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kAmiSetMdrPos,    ipmi::Privilege::Admin, ipmiAmiSetMdrPos);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kAmiGetMdrStatus, ipmi::Privilege::Admin, ipmiAmiGetMdrStatus);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kAmiSetBiosInfo,  ipmi::Privilege::Admin, ipmiAmiSetBiosInfo);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kAmiSetSmbiosChunk, ipmi::Privilege::Admin, ipmiAmiSetSmbiosChunk);
        ipmi::registerHandler(ipmi::prioOemBase, mdrNetFn,
                              kAmiGetStatus,    ipmi::Privilege::Admin, ipmiAmiGetStatus);
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock MDR SMBIOS hooks registered (prioOemBase, NetFn 0x32+0x3A)");

    // ── Probe handlers for protocol discovery ──────────────────────────
    // Registered at prioOpenBmcBase (< prioOemBase) so real handlers above
    // always win. Catches any command the BIOS sends that isn't explicitly
    // handled — logs the raw bytes so we can identify unknown protocol steps.
    // Excludes commands already handled at prioOemBase (they'd never reach here).
    static const uint8_t kProbeCmds[] = {
        // Low range (excluding YAFU 0x01-0x10 which have stubs at prioOemBase)
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x2A, 0x2C, 0x2D, 0x2E, 0x2F,
        // MDR range (excluding 0x30 GetSelPolicy, 0x31 GetDir, 0x3D GetMdrStatus)
        0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3A, 0x3B, 0x3C, 0x3E, 0x3F,
        // 0x40-0x50 range
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50,
        // 0x54-0x5F (excl 0x51 WriteBegin, 0x52 WriteChunk, 0x53 WriteEnd, 0x5D LegacyCtrl)
        0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F,
        // 0x60-0x70 (excl 0x71 RegionStatus, 0x72 GetBlock)
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70,
        // 0x73-0x9F
        0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
        0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
        0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
        // 0xA2-0xB1, 0xB3-0xB4, 0xB6-0xF2, 0xF4-0xFE
        // (excl 0xA0 SetMdrPos, 0xA1 GetMdrStatus, 0xB2 SetBiosInfo, 0xB5 SetSmbiosChunk,
        //       0xF3 GetStatus, 0xE6 Inventory, 0xE9 Peci, 0xEC Psu, 0xEE Mux)
        0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9,
        0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
        0xB0, 0xB1, 0xB3, 0xB4,
        0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
        0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
        0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
        0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
        0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
        0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE7, 0xE8,
        0xEA, 0xEB, 0xED, 0xEF,
        0xF0, 0xF1, 0xF2, 0xF4, 0xF5, 0xF6, 0xF7,
        0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
    };
    for (auto probeNetFn : {static_cast<ipmi::NetFn>(ipmi::netFnOemTwo),
                             static_cast<ipmi::NetFn>(ipmi::netFnOemSix)})
    {
        for (uint8_t c : kProbeCmds)
        {
            ipmi::registerHandler(ipmi::prioOpenBmcBase, probeNetFn,
                                  static_cast<ipmi::Cmd>(c),
                                  ipmi::Privilege::Admin, ipmiProbeCmdHandler);
        }
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock OEM probe handlers registered (prioOpenBmcBase, NetFn 0x32+0x3A)");
}

} // namespace asrock
