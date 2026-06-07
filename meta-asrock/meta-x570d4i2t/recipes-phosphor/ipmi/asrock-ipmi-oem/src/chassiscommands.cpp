// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Chassis NetFn (0x00) command overrides for the X570D4I-2T.
//
// Overrides phosphor-ipmi-host defaults at prioOemBase:
//
//   GetChassisStatus (0x01) — adds chassis intrusion state sourced from the
//     SIO chassis-intrusion sensor (NCT6796D at SIO 0x90, mapped to the D-Bus
//     Chassis.Intrusion interface) and identify LED status.
//
//   ChassisIdentify (0x04) — controls the identify LED group
//     /xyz/openbmc_project/led/groups/enclosure_identify.
//
//   GetSystemRestartCause (0x07) — maps the D-Bus State.Host RestartCause
//     enumeration to the IPMI §28.17 cause codes.
//
// Method references:
//   bmc-analyze §NETFN_CHASSIS (g_Chassis_CmdHndlr 0x01/0x04/0x07)
//   bmc-analyze §Discrete/Status Sensors:
//     dev_asrr_chassisintr_sio_v01_0x90h — chassis intrusion via SIO 0x90
//   bmc-analyze §OpenBMC Port Implications: sync agent Chassis.get_chassis_status

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <chrono>
#include <string>
#include <variant>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus constants
// -----------------------------------------------------------------------

// Chassis / Host power state
static constexpr const char* chassisStateSvc  =
    "xyz.openbmc_project.State.Chassis";
static constexpr const char* chassisStateObj  =
    "/xyz/openbmc_project/state/chassis0";
static constexpr const char* chassisStateIntf =
    "xyz.openbmc_project.State.Chassis";
static constexpr const char* chassisPowerStateProp = "CurrentPowerState";
static constexpr const char* chassisPowerOn =
    "xyz.openbmc_project.State.Chassis.PowerState.On";

// Power restore policy
static constexpr const char* powerRestoreIntf =
    "xyz.openbmc_project.Control.Power.RestorePolicy";
static constexpr const char* powerRestoreProp   = "PowerRestorePolicy";
static constexpr const char* restorePolicyOff   =
    "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOff";
static constexpr const char* restorePolicyOn    =
    "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOn";
static constexpr const char* restorePolicyPrev  =
    "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.Restore";

// Chassis intrusion — NCT6796D SIO 0x90 mapped to D-Bus
// (dev_asrr_chassisintr_sio_v01_0x90h_0_32 per bmc-analyze §Discrete Sensors)
static constexpr const char* intrusionIntf =
    "xyz.openbmc_project.Chassis.Intrusion";
static constexpr const char* intrusionObj  =
    "/xyz/openbmc_project/Chassis/Intrusion";
static constexpr const char* intrusionProp = "Status";
static constexpr const char* intrusionActive = "HardwareIntrusion";

// Identify LED group (enclosure identify)
static constexpr const char* ledGroupSvc  =
    "xyz.openbmc_project.LED.GroupManager";
static constexpr const char* identifyLedObj =
    "/xyz/openbmc_project/led/groups/enclosure_identify";
static constexpr const char* ledGroupIntf =
    "xyz.openbmc_project.Led.Group";
static constexpr const char* ledAssertedProp = "Asserted";

// Host restart cause
static constexpr const char* hostStateObj  =
    "/xyz/openbmc_project/state/host0";
static constexpr const char* hostStateIntf =
    "xyz.openbmc_project.State.Host";
static constexpr const char* restartCauseProp = "RestartCause";

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static void registerChassisCommands() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helper: query a D-Bus property, return default on any failure
// -----------------------------------------------------------------------

template <typename T>
static T dbusGet(const char* svc, const char* obj, const char* intf,
                 const char* prop, T defaultVal)
{
    try
    {
        auto dbus = getSdBus();
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, obj, intf, prop);
        return std::get<T>(v);
    }
    catch (...) { return defaultVal; }
}

// -----------------------------------------------------------------------
// GetChassisStatus (NetFn Chassis / 0x01)
// Confirmed: bmc-analyze §NETFN_CHASSIS 0x01, privilege USER
//
// Response layout (IPMI spec §28.2):
//   Byte 1 — Current power state
//     [7:6] power restore policy
//     [5]   power control fault
//     [4]   power fault
//     [3]   interlock active
//     [2]   power overload
//     [1]   power domain
//     [0]   power on
//   Byte 2 — Last power event
//   Byte 3 — Misc chassis state
//     [6:5] identify state (00=off, 01=temp, 10=indef)
//     [3]   cooling/fan fault
//     [2]   drive fault
//     [1]   front-panel lockout active
//     [0]   chassis intrusion active
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint8_t, uint8_t>
    ipmiGetChassisStatus(ipmi::Context::ptr& /*ctx*/)
{
    uint8_t powerState = 0;
    uint8_t lastPowerEvent = 0;
    uint8_t miscState = 0;

    // ---- Current power state ----
    bool powerOn = false;
    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, chassisStateIntf,
                                            chassisStateObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, chassisStateObj,
                                               chassisStateIntf,
                                               chassisPowerStateProp);
        powerOn = (std::get<std::string>(v) == chassisPowerOn);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "GetChassisStatus: power state lookup failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }
    if (powerOn) powerState |= (1 << 0);

    // ---- Power restore policy bits [7:5] ----
    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, powerRestoreIntf,
                                            chassisStateObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, chassisStateObj,
                                               powerRestoreIntf,
                                               powerRestoreProp);
        const auto& policy = std::get<std::string>(v);
        if (policy == restorePolicyOff)
            powerState |= (0b00 << 5);
        else if (policy == restorePolicyOn)
            powerState |= (0b10 << 5);
        else if (policy == restorePolicyPrev)
            powerState |= (0b01 << 5);
    }
    catch (...) {}

    // ---- Chassis intrusion (bit 0 of miscState) ----
    // NCT6796D SIO 0x90 chassis intrusion sensor per bmc-analyze §Discrete Sensors
    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, intrusionIntf, intrusionObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, intrusionObj,
                                               intrusionIntf, intrusionProp);
        const auto& intrusionStatus = std::get<std::string>(v);
        if (intrusionStatus == intrusionActive ||
            intrusionStatus.find("Intrusion") != std::string::npos)
        {
            miscState |= (1 << 0);
        }
    }
    catch (...) {} // Intrusion sensor may not be populated — safe to ignore

    // ---- Identify LED state (bits [6:5] of miscState) ----
    // bit7=1 means identify command supported; bits 6:4 encode state
    miscState |= (1 << 6); // Identify LED supported
    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, ledGroupIntf, identifyLedObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, identifyLedObj,
                                               ledGroupIntf, ledAssertedProp);
        bool asserted = std::get<bool>(v);
        if (asserted)
        {
            // Indefinite identify (0b10 in bits 6:5)
            miscState |= (0b10 << 4);
        }
    }
    catch (...) {}

    return ipmi::responseSuccess(powerState, lastPowerEvent, miscState);
}

// -----------------------------------------------------------------------
// ChassisIdentify (NetFn Chassis / 0x04)
// Confirmed: bmc-analyze §NETFN_CHASSIS 0x04 SetChassisIdentity, privilege OPERATOR
//
// Request:  [Byte 0] identify interval (seconds; 0=off, 0xFF=indef)
//           [Byte 1] force identify (optional; 1=force on)
//
// Controls /xyz/openbmc_project/led/groups/enclosure_identify Asserted.
// -----------------------------------------------------------------------

ipmi::RspType<>
    ipmiChassisIdentify(ipmi::Context::ptr& /*ctx*/,
                        std::optional<uint8_t> interval,
                        std::optional<uint8_t> forceIdentify)
{
    bool enable = true;

    if (interval && *interval == 0)
    {
        enable = false;
    }
    if (forceIdentify && *forceIdentify == 0)
    {
        // force=0 means use interval; enable already set above
    }

    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, ledGroupIntf, identifyLedObj);
        ipmi::setDbusProperty(*dbus, svc, identifyLedObj, ledGroupIntf,
                              ledAssertedProp, enable);

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "ipmiChassisIdentify: identify LED set",
            phosphor::logging::entry("ENABLED=%d", enable));
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiChassisIdentify: LED control failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// GetSystemRestartCause (NetFn Chassis / 0x07)
// Confirmed: bmc-analyze §NETFN_CHASSIS 0x07, privilege USER
//
// Response: [Byte 0] cause code (IPMI spec §28.17)
//           [Byte 1] channel number (0)
//
// D-Bus RestartCause → IPMI cause mapping:
//   IpmiCommand       → 0x01 chassis control command
//   ResetButton       → 0x02 reset via pushbutton
//   PowerButton       → 0x03 power-up via pushbutton
//   WatchdogTimer     → 0x04 watchdog expired
//   PowerPolicyAlwaysOn / PowerPolicyPreviousState → 0x06/0x07
//   SoftReset         → 0x0A soft reset
//   Unknown           → 0x00
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint8_t>
    ipmiGetSystemRestartCause(ipmi::Context::ptr& /*ctx*/)
{
    uint8_t cause = 0x00; // unknown

    try
    {
        auto dbus = getSdBus();
        std::string svc = ipmi::getService(*dbus, hostStateIntf, hostStateObj);
        ipmi::Value v = ipmi::getDbusProperty(*dbus, svc, hostStateObj,
                                               hostStateIntf, restartCauseProp);
        const auto& causeStr = std::get<std::string>(v);

        if (causeStr.find("IpmiCommand") != std::string::npos)
            cause = 0x01;
        else if (causeStr.find("ResetButton") != std::string::npos)
            cause = 0x02;
        else if (causeStr.find("PowerButton") != std::string::npos)
            cause = 0x03;
        else if (causeStr.find("WatchdogTimer") != std::string::npos)
            cause = 0x04;
        else if (causeStr.find("AlwaysOn") != std::string::npos)
            cause = 0x06;
        else if (causeStr.find("PreviousState") != std::string::npos ||
                 causeStr.find("Restore") != std::string::npos)
            cause = 0x07;
        else if (causeStr.find("SoftReset") != std::string::npos)
            cause = 0x0A;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetSystemRestartCause: D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    return ipmi::responseSuccess(cause, static_cast<uint8_t>(0));
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerChassisCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock Chassis commands registered (NetFn 0x00)");

    // GetChassisStatus (0x01) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnChassis,
                          static_cast<ipmi::Cmd>(0x01),
                          ipmi::Privilege::User, ipmiGetChassisStatus);

    // ChassisIdentify (0x04) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnChassis,
                          static_cast<ipmi::Cmd>(0x04),
                          ipmi::Privilege::Operator, ipmiChassisIdentify);

    // GetSystemRestartCause (0x07) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnChassis,
                          static_cast<ipmi::Cmd>(0x07),
                          ipmi::Privilege::User, ipmiGetSystemRestartCause);
}

} // namespace asrock
