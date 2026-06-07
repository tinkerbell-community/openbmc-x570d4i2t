// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Storage NetFn (0x0A) command overrides for the X570D4I-2T.
//
// AddSELEntry (0x44) — receives a 16-byte SEL record from the BIOS or OS,
//   parses it, and routes it to xyz.openbmc_project.Logging.Create so that
//   entries appear in the Redfish EventLog
//   (/redfish/v1/Systems/system/LogServices/EventLog/Entries).
//   Replaces the phosphor-ipmi-host file-backed SEL with a Redfish-native path.
//
// GetSELTime (0x48) — returns the current wall-clock time so that the BIOS
//   can timestamp SEL records correctly.
//
// GetSELInfo (0x40) — returns SEL capacity and overflow status, sourced from
//   the phosphor-logging entry count.
//
// Method references:
//   bmc-analyze §NETFN_STORAGE (g_Storage_CmdHndlr, codes 0x40/0x43/0x44/0x47/0x48)
//   bmc-analyze §Sync Agent IPMI→Redfish:
//     NETFN_STORAGE 0x47 ClearSEL → storage.clear_SEL_entries
//     NETFN_STORAGE 0x46 DeleteSEL → storage.delete_SEL_entry

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <chrono>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus constants
// -----------------------------------------------------------------------

static constexpr const char* logSvc    = "xyz.openbmc_project.Logging";
static constexpr const char* logObj    = "/xyz/openbmc_project/logging";
static constexpr const char* logCreateIntf    =
    "xyz.openbmc_project.Logging.Create";
static constexpr const char* logDeleteIntf    =
    "xyz.openbmc_project.Logging";
static constexpr const char* logEntryIntf     =
    "xyz.openbmc_project.Logging.Entry";

// SEL entry limits — Redfish EventLog is unbounded but we report a plausible
// maximum to satisfy IPMI spec §31.4 GetSELInfo.
static constexpr uint16_t maxSelEntries = 3000;

// -----------------------------------------------------------------------
// Forward declaration
// -----------------------------------------------------------------------

static void registerStorageCommands() __attribute__((constructor));

// -----------------------------------------------------------------------
// SEL record type constants (IPMI spec §31.6.1)
// -----------------------------------------------------------------------

static constexpr uint8_t selRecTypeSystem   = 0x02; // system event
static constexpr uint8_t selRecTypeTimestmp = 0x03; // timestamped OEM
static constexpr uint8_t selRecTypeNonTmstm = 0xFF; // non-timestamped OEM

// -----------------------------------------------------------------------
// Map IPMI SEL record sensor type to a phosphor-logging severity level.
// Same mapping as sensorcommands.cpp PlatformEvent, kept in sync.
// -----------------------------------------------------------------------

static std::string selSeverity(uint8_t sensorType, bool assertion)
{
    if (!assertion)
        return "xyz.openbmc_project.Logging.Entry.Level.Informational";

    switch (sensorType)
    {
        case 0x07: case 0x12:
            return "xyz.openbmc_project.Logging.Entry.Level.Critical";
        case 0x01: case 0x08: case 0x0F: case 0x23:
            return "xyz.openbmc_project.Logging.Entry.Level.Error";
        case 0x04: case 0x0A: case 0x17: case 0x1D:
            return "xyz.openbmc_project.Logging.Entry.Level.Warning";
        default:
            return "xyz.openbmc_project.Logging.Entry.Level.Informational";
    }
}

// -----------------------------------------------------------------------
// GetSELInfo (NetFn Storage / 0x40)
// Confirmed: bmc-analyze §NETFN_STORAGE 0x40, USER
//
// Response (IPMI spec §31.4):
//   [0]     SEL version (0x51 = IPMI v1.5)
//   [1-2]   number of log entries (LE)
//   [3-4]   free space in bytes (LE) — approximate
//   [5-8]   most recent addition timestamp (LE)
//   [9-12]  most recent erase timestamp (LE)
//   [13]    operation support flags
//            bit0 = get SEL allocation info supported
//            bit1 = reserve SEL supported
//            bit2 = partial add SEL supported
//            bit3 = delete SEL supported
//            bit7 = overflow flag
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t,   // selVersion
              uint16_t,  // entries
              uint16_t,  // freeBytes
              uint32_t,  // addTimestamp
              uint32_t,  // eraseTimestamp
              uint8_t>   // operationSupport
    ipmiStorageGetSELInfo(ipmi::Context::ptr& /*ctx*/)
{
    uint16_t entryCount = 0;
    uint32_t addTs  = 0;
    uint32_t eraseTs = 0;

    // Count phosphor-logging entries (proxy for SEL size)
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(logSvc, logObj,
                                          "org.freedesktop.DBus.Properties",
                                          "Get");
        // GetManagedObjects to count entries
        auto entriesMsg = dbus->new_method_call(logSvc, logObj,
                                                 "org.freedesktop.DBus.ObjectManager",
                                                 "GetManagedObjects");
        auto reply = dbus->call(entriesMsg);
        std::map<sdbusplus::message::object_path,
                 std::map<std::string,
                          std::map<std::string, ipmi::Value>>> objs;
        reply.read(objs);
        for (const auto& [path, ifaces] : objs)
        {
            if (ifaces.count(logEntryIntf)) ++entryCount;
        }
    }
    catch (...) {}

    uint8_t opSupport = 0x0F; // get-alloc-info + reserve + partial-add + delete
    if (entryCount >= maxSelEntries) opSupport |= (1 << 7); // overflow

    // Free space approximation (16 bytes/entry in classic SEL terms)
    uint16_t freeBytes = static_cast<uint16_t>(
        (maxSelEntries > entryCount)
            ? (maxSelEntries - entryCount) * 16
            : 0);

    return ipmi::responseSuccess(
        static_cast<uint8_t>(0x51), entryCount, freeBytes,
        addTs, eraseTs, opSupport);
}

// -----------------------------------------------------------------------
// AddSELEntry (NetFn Storage / 0x44)
// Confirmed: bmc-analyze §NETFN_STORAGE 0x44, OPERATOR
// Sync agent mapping: NETFN_STORAGE 0x44 routes to storage.get_FRU_Info (via
//   Redis pub/sub), confirming entries must reach the Redfish path.
//
// Request: 16-byte SEL record
//   [0-1]   record ID (host-supplied, we ignore and generate our own)
//   [2]     record type
//   [3-6]   timestamp (seconds since epoch, LE)
//   [7-8]   generator ID (LE)
//   [9]     EvMRev
//   [10]    sensor type
//   [11]    sensor number
//   [12]    event dir | event type
//   [13-15] event data 1/2/3
//
// Response: [0-1] assigned record ID (LE)
// -----------------------------------------------------------------------

ipmi::RspType<uint16_t>
    ipmiStorageAddSELEntry(ipmi::Context::ptr& /*ctx*/,
                            std::vector<uint8_t> record)
{
    if (record.size() < 16)
    {
        return ipmi::responseReqDataLenInvalid();
    }

    uint8_t  recType    = record[2];
    uint8_t  sensorType = record[10];
    uint8_t  sensorNum  = record[11];
    uint8_t  evtDirType = record[12];
    uint8_t  evtData1   = record[13];
    uint8_t  evtData2   = record[14];
    uint8_t  evtData3   = record[15];

    bool assertion = !(evtDirType & 0x80);
    uint8_t eventType = evtDirType & 0x7F;

    // Build additional data map
    std::map<std::string, std::string> addData;
    char hexBuf[16];

    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", recType);
    addData["SEL_RECORD_TYPE"] = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", sensorType);
    addData["SENSOR_TYPE"]   = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", sensorNum);
    addData["SENSOR_NUM"]    = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", eventType);
    addData["EVENT_TYPE"]    = hexBuf;
    addData["EVENT_DIR"]     = assertion ? "assertion" : "deassertion";
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", evtData1);
    addData["EVENT_DATA1"]   = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", evtData2);
    addData["EVENT_DATA2"]   = hexBuf;
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%02X", evtData3);
    addData["EVENT_DATA3"]   = hexBuf;

    // Generator ID as two bytes LE
    uint16_t genId = static_cast<uint16_t>(record[7] | (record[8] << 8));
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%04X", genId);
    addData["GENERATOR_ID"]  = hexBuf;

    char msgBuf[128];
    std::snprintf(msgBuf, sizeof(msgBuf),
                  "IPMI SEL entry: sensorType=0x%02X sensorNum=0x%02X "
                  "eventType=0x%02X dir=%s data=0x%02X/0x%02X/0x%02X",
                  sensorType, sensorNum, eventType,
                  assertion ? "assert" : "deassert",
                  evtData1, evtData2, evtData3);

    std::string severity = selSeverity(sensorType, assertion);
    uint16_t newRecordId = 0xFFFF; // no-persistence placeholder

    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(logSvc, logObj,
                                          logCreateIntf, "Create");
        msg.append(std::string(msgBuf), severity, addData);
        dbus->call_noreply(msg);
        // Record ID: use current time as a monotonic proxy
        newRecordId = static_cast<uint16_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count() & 0xFFFF);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiStorageAddSELEntry: logging failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    return ipmi::responseSuccess(newRecordId);
}

// -----------------------------------------------------------------------
// GetSELTime (NetFn Storage / 0x48)
// Confirmed: bmc-analyze §NETFN_STORAGE 0x48, USER
//
// Response: uint32_t — seconds since 1970-01-01 00:00:00 UTC (IPMI epoch)
// -----------------------------------------------------------------------

ipmi::RspType<uint32_t> ipmiStorageGetSELTime(ipmi::Context::ptr& /*ctx*/)
{
    auto now = std::chrono::system_clock::now();
    uint32_t epochSeconds = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count());
    return ipmi::responseSuccess(epochSeconds);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerStorageCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock Storage commands registered (NetFn 0x0A)");

    // GetSELInfo (0x40) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x40),
                          ipmi::Privilege::User, ipmiStorageGetSELInfo);

    // AddSELEntry (0x44) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x44),
                          ipmi::Privilege::Operator, ipmiStorageAddSELEntry);

    // GetSELTime (0x48) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x48),
                          ipmi::Privilege::User, ipmiStorageGetSELTime);
}

} // namespace asrock
