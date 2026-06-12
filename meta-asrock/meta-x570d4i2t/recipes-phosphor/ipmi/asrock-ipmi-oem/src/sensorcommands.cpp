// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Sensor/Event NetFn (0x04) command overrides for the X570D4I-2T.
//
// PlatformEvent (0x02) — receives discrete sensor / threshold events from the
//   host BIOS (via KCS1/SMM channel) or OS and routes them to the
//   xyz.openbmc_project.Logging service so they appear in the Redfish
//   EventLog (/redfish/v1/Systems/system/LogServices/EventLog/Entries).
//
// Method references:
//   bmc-analyze §NETFN_SENSOR (g_SensorEvent_CmdHndlr, code 0x02, OPERATOR)
//   bmc-analyze §Sync Agent IPMI→Redfish: sensors.update_power_thermal
//   bmc-analyze §Discrete/Status Sensors: watchdog (0xF9), power-unit (0xFA),
//     chassis-intrusion SIO (0x90) — these sensors post platform events during
//     POST via KCS1/SMM channel.

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <map>
#include <string>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus constants
// -----------------------------------------------------------------------

static constexpr const char* loggingSvc  = "xyz.openbmc_project.Logging";
static constexpr const char* loggingObj  = "/xyz/openbmc_project/logging";
static constexpr const char* loggingCreateIntf =
    "xyz.openbmc_project.Logging.Create";
static constexpr const char* loggingCreateMethod = "Create";

// -----------------------------------------------------------------------
// Forward declaration
// -----------------------------------------------------------------------

static void registerSensorCommands() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Map IPMI event direction + sensor type to a phosphor-logging severity string.
// IPMI event direction: bit7 of event type byte
//   0 = assertion, 1 = deassertion
// Sensor types mapped to severities from bmc-analyze §Discrete/Status Sensors:
//   0x01 Temperature         → Warning/Error
//   0x04 Fan                 → Warning
//   0x07 Processor           → Critical
//   0x08 Power Supply        → Error (PSU 0xFA)
//   0x0A Battery             → Warning
//   0x0F System Firmware     → Error
//   0x12 OS Critical Stop    → Critical
//   0x17 Slot/Connector      → Warning
//   0x1D Management Subsys   → Warning
//   0x20 OS Boot             → Informational
//   0x23 Watchdog2           → Error (watchdog 0xF9)

static std::string eventSeverity(uint8_t sensorType, bool assertion)
{
    if (!assertion)
    {
        // Deassertion is always informational (condition cleared)
        return "xyz.openbmc_project.Logging.Entry.Level.Informational";
    }

    switch (sensorType)
    {
        case 0x07: // Processor
        case 0x12: // OS Critical Stop
            return "xyz.openbmc_project.Logging.Entry.Level.Critical";

        case 0x01: // Temperature
        case 0x08: // Power Supply
        case 0x0F: // System Firmware
        case 0x23: // Watchdog2
            return "xyz.openbmc_project.Logging.Entry.Level.Error";

        case 0x04: // Fan
        case 0x0A: // Battery
        case 0x17: // Slot/Connector
        case 0x1D: // Management Subsystem Health
            return "xyz.openbmc_project.Logging.Entry.Level.Warning";

        default:
            return "xyz.openbmc_project.Logging.Entry.Level.Informational";
    }
}

// Produce a human-readable message string for the log entry.
static std::string eventMessage(uint8_t sensorType, uint8_t sensorNum,
                                 uint8_t eventType, uint8_t eventData1)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "IPMI platform event: sensorType=0x%02X sensorNum=0x%02X "
                  "eventType=0x%02X eventData1=0x%02X",
                  sensorType, sensorNum, eventType, eventData1);
    return {buf};
}

// -----------------------------------------------------------------------
// PlatformEvent (NetFn Sensor / 0x02)
// Confirmed: bmc-analyze §NETFN_SENSOR 0x02 CMD_PLATFORM_EVENT, OPERATOR
//
// Request bytes (IPMI spec §29.3):
//   [0] EvMRev   — event message format revision (0x04)
//   [1] SensorType
//   [2] SensorNum
//   [3] EventDir | EventType  (bit7 = direction: 0=assert, 1=deassert)
//   [4] EventData1
//   [5] EventData2 (optional)
//   [6] EventData3 (optional)
//
// Routes the event to xyz.openbmc_project.Logging.Create so that bmcweb
// publishes it to /redfish/v1/Systems/system/LogServices/EventLog/Entries.
// -----------------------------------------------------------------------

ipmi::RspType<>
    ipmiSenPlatformEvent(ipmi::Context::ptr& /*ctx*/,
                         uint8_t evMRev,
                         uint8_t sensorType,
                         uint8_t sensorNum,
                         uint8_t eventDirType,
                         uint8_t eventData1,
                         std::optional<uint8_t> eventData2,
                         std::optional<uint8_t> eventData3)
{
    // Validate event message revision
    if (evMRev != 0x04)
    {
        return ipmi::responseInvalidFieldRequest();
    }

    bool assertion = !(eventDirType & 0x80);
    uint8_t eventType = eventDirType & 0x7F;

    // Build additional data map for phosphor-logging
    std::map<std::string, std::string> addData;
    char hexBuf[8];
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", sensorType);
    addData["SENSOR_TYPE"]  = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", sensorNum);
    addData["SENSOR_NUM"]   = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", eventType);
    addData["EVENT_TYPE"]   = hexBuf;
    addData["EVENT_DIR"]    = assertion ? "assertion" : "deassertion";
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", eventData1);
    addData["EVENT_DATA1"]  = hexBuf;
    if (eventData2)
    {
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", *eventData2);
        addData["EVENT_DATA2"] = hexBuf;
    }
    if (eventData3)
    {
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", *eventData3);
        addData["EVENT_DATA3"] = hexBuf;
    }

    std::string severity  = eventSeverity(sensorType, assertion);
    std::string message   = eventMessage(sensorType, sensorNum,
                                          eventType, eventData1);

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "PlatformEvent",
        phosphor::logging::entry("SENSOR_TYPE=0x%02X", sensorType),
        phosphor::logging::entry("SENSOR_NUM=0x%02X", sensorNum),
        phosphor::logging::entry("EVENT_TYPE=0x%02X", eventType),
        phosphor::logging::entry("DIR=%s", assertion ? "assert" : "deassert"));

    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(loggingSvc, loggingObj,
                                          loggingCreateIntf,
                                          loggingCreateMethod);
        msg.append(message, severity, addData);
        dbus->call_noreply(msg);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiSenPlatformEvent: logging failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        // Return success anyway — the BIOS must not retry indefinitely
        // because the BMC logging path is unavailable.
    }

    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerSensorCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock Sensor commands registered (NetFn 0x04)");

    // PlatformEvent (0x02) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnSensor,
                          static_cast<ipmi::Cmd>(0x02),
                          ipmi::Privilege::Admin, ipmiSenPlatformEvent);
}

} // namespace asrock
