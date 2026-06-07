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

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <optional>
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
// Map IPMI SEL sensor type to a Redfish OriginOfCondition path.
// Derived from sel2redfish_handler.lua (AMI firmware v01.91.00).
// -----------------------------------------------------------------------

static std::string selOriginOfCondition(uint8_t sensorType)
{
    switch (sensorType)
    {
        case 0x01: case 0x04:
            return "/redfish/v1/Chassis/system/Thermal";
        case 0x02: case 0x03: case 0x08: case 0x09:
            return "/redfish/v1/Chassis/system/Power";
        case 0x07:
            return "/redfish/v1/Systems/system/Processors";
        case 0x0C:
            return "/redfish/v1/Systems/system/Memory";
        case 0x0F: case 0x12: case 0x1D: case 0x1E: case 0x1F:
        case 0x20: case 0x21: case 0x22:
            return "/redfish/v1/Systems/system";
        case 0x18: case 0x05:
            return "/redfish/v1/Chassis/system";
        default:
            return "/redfish/v1/Managers/bmc";
    }
}

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

    // Redfish OriginOfCondition: sensor type → resource path
    // Mapping derived from sel2redfish_handler.lua (AMI firmware v01.91.00)
    addData["REDFISH_ORIGIN_OF_CONDITION"] = selOriginOfCondition(sensorType);

    char msgBuf[128];
    std::snprintf(msgBuf, sizeof(msgBuf),
                  "IPMI SEL entry: sensorType=0x%02X sensorNum=0x%02X "
                  "eventType=0x%02X dir=%s data=0x%02X/0x%02X/0x%02X",
                  sensorType, sensorNum, eventType,
                  assertion ? "assert" : "deassert",
                  evtData1, evtData2, evtData3);

    std::string severity = selSeverity(sensorType, assertion);
    uint16_t newRecordId = 0xFFFF; // no-persistence placeholder

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "AddSELEntry",
        phosphor::logging::entry("SENSOR_TYPE=0x%02X", sensorType),
        phosphor::logging::entry("SENSOR_NUM=0x%02X", sensorNum),
        phosphor::logging::entry("EVENT_TYPE=0x%02X", eventType),
        phosphor::logging::entry("DIR=%s", assertion ? "assert" : "deassert"));

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
// GetSELEntry (NetFn Storage / 0x43)
// Confirmed: bmc-analyze §NETFN_STORAGE 0x43, USER
//
// Request:
//   [0-1] reservationId (LE, ignored — no reservation support yet)
//   [2-3] recordId (LE): 0x0000 = first, 0xFFFF = last
//   [4]   offset (must be 0 for whole-record fetch)
//   [5]   bytesToRead (0xFF = all)
//
// Response:
//   [0-1] nextRecordId (LE): 0xFFFF if no more records
//   [2-17] 16-byte SEL record reconstructed from phosphor-logging entry
//
// Record format (IPMI spec §31.6.1 system event record):
//   [0-1]  recordId (LE)
//   [2]    recordType: 0x02 (system event)
//   [3-6]  timestamp: seconds since epoch (LE)
//   [7-8]  generatorId: 0x0020 = BMC (LE)
//   [9]    EvMRev: 0x04
//   [10]   sensorType  (from AdditionalData SENSOR_TYPE)
//   [11]   sensorNum   (from AdditionalData SENSOR_NUM)
//   [12]   eventDir | eventType (from AdditionalData)
//   [13-15] eventData 1/2/3
// -----------------------------------------------------------------------

ipmi::RspType<uint16_t, std::vector<uint8_t>>
    ipmiStorageGetSELEntry(ipmi::Context::ptr& /*ctx*/,
                           uint16_t /*reservationId*/,
                           uint16_t recordId,
                           uint8_t  /*offset*/,
                           uint8_t  /*bytesToRead*/)
{
    using ObjMap = std::map<sdbusplus::message::object_path,
                            std::map<std::string,
                                     std::map<std::string, ipmi::Value>>>;
    ObjMap objs;
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(logSvc, logObj,
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects");
        auto reply = dbus->call(msg);
        reply.read(objs);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiStorageGetSELEntry: GetManagedObjects failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    // Collect entry IDs sorted ascending
    std::vector<uint32_t> ids;
    ids.reserve(objs.size());
    for (const auto& [path, ifaces] : objs)
    {
        if (!ifaces.count(logEntryIntf)) continue;
        auto& props = ifaces.at(logEntryIntf);
        auto it = props.find("Id");
        if (it == props.end()) continue;
        ids.push_back(std::get<uint32_t>(it->second));
    }
    std::sort(ids.begin(), ids.end());

    if (ids.empty())
        return ipmi::responseResponseError(); // SEL empty

    uint32_t target = 0;
    if (recordId == 0x0000)
        target = ids.front();
    else if (recordId == 0xFFFF)
        target = ids.back();
    else
        target = recordId;

    // Find target in sorted list
    auto tIt = std::find(ids.begin(), ids.end(), target);
    if (tIt == ids.end())
        return ipmi::responseSensorInvalid(); // no such record

    uint16_t nextId = 0xFFFF;
    if (std::next(tIt) != ids.end())
        nextId = static_cast<uint16_t>(*std::next(tIt));

    // Fetch properties of the target entry
    std::string entryPath = std::string(logObj) + "/entry/" +
                            std::to_string(target);
    std::map<std::string, ipmi::Value> props;
    auto& ifaces = objs.at(sdbusplus::message::object_path(entryPath));
    if (ifaces.count(logEntryIntf))
        props = ifaces.at(logEntryIntf);

    // Reconstruct 16-byte SEL record
    uint16_t recId     = static_cast<uint16_t>(target & 0xFFFF);
    uint64_t tsMs      = 0;
    if (auto it = props.find("Timestamp"); it != props.end())
        tsMs = std::get<uint64_t>(it->second);
    uint32_t tsSec = static_cast<uint32_t>(tsMs / 1000);

    // Parse AdditionalData key=value pairs
    uint8_t sensorType = 0xFF;
    uint8_t sensorNum  = 0xFF;
    uint8_t evtDirType = 0x00;
    uint8_t evtData1   = 0xFF;
    uint8_t evtData2   = 0xFF;
    uint8_t evtData3   = 0xFF;

    auto parseHex = [](const std::string& s) -> uint8_t {
        try { return static_cast<uint8_t>(std::stoul(s, nullptr, 16)); }
        catch (...) { return 0xFF; }
    };

    if (auto it = props.find("AdditionalData"); it != props.end())
    {
        for (const auto& kv : std::get<std::vector<std::string>>(it->second))
        {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string key = kv.substr(0, eq);
            std::string val = kv.substr(eq + 1);
            if (key == "SENSOR_TYPE")  sensorType = parseHex(val);
            else if (key == "SENSOR_NUM") sensorNum = parseHex(val);
            else if (key == "EVENT_TYPE") evtDirType = parseHex(val);
            else if (key == "EVENT_DIR" && val == "deassertion")
                evtDirType |= 0x80;
            else if (key == "EVENT_DATA1") evtData1 = parseHex(val);
            else if (key == "EVENT_DATA2") evtData2 = parseHex(val);
            else if (key == "EVENT_DATA3") evtData3 = parseHex(val);
        }
    }

    std::vector<uint8_t> selRecord = {
        static_cast<uint8_t>(recId & 0xFF),
        static_cast<uint8_t>((recId >> 8) & 0xFF),
        0x02, // system event record type
        static_cast<uint8_t>(tsSec & 0xFF),
        static_cast<uint8_t>((tsSec >> 8) & 0xFF),
        static_cast<uint8_t>((tsSec >> 16) & 0xFF),
        static_cast<uint8_t>((tsSec >> 24) & 0xFF),
        0x20, 0x00, // generator ID: BMC (LE)
        0x04,       // EvMRev
        sensorType,
        sensorNum,
        evtDirType,
        evtData1,
        evtData2,
        evtData3
    };

    return ipmi::responseSuccess(nextId, selRecord);
}

// -----------------------------------------------------------------------
// ClearSEL (NetFn Storage / 0x47)
// Confirmed: bmc-analyze §NETFN_STORAGE 0x47, OPERATOR
// Sync agent mapping: storage.clear_SEL_entries
//
// Request:
//   [0-1] reservationId (LE, ignored — no reservation support)
//   [2-4] "CLR" (ASCII 0x43 0x4C 0x52) — sanity check
//   [5]   action: 0xAA = initiate erase, 0x00 = get erase status
//
// Response:
//   [0]   erasure progress: 0xFF = erase completed, 0x00 = in progress
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t>
    ipmiStorageClearSEL(ipmi::Context::ptr& /*ctx*/,
                        uint16_t /*reservationId*/,
                        uint8_t  clr0, uint8_t clr1, uint8_t clr2,
                        uint8_t  action)
{
    // Validate "CLR" magic bytes
    if (clr0 != 'C' || clr1 != 'L' || clr2 != 'R')
        return ipmi::responseInvalidFieldRequest();

    if (action == 0x00)
    {
        // Status query: always report complete (no async erase)
        return ipmi::responseSuccess(static_cast<uint8_t>(0xFF));
    }

    if (action != 0xAA)
        return ipmi::responseInvalidFieldRequest();

    // Collect all entry paths and delete each one
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ClearSEL: erasing all log entries");
    uint32_t deletedCount = 0;
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(logSvc, logObj,
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects");
        auto reply = dbus->call(msg);
        std::map<sdbusplus::message::object_path,
                 std::map<std::string,
                          std::map<std::string, ipmi::Value>>> objs;
        reply.read(objs);

        for (const auto& [path, ifaces] : objs)
        {
            if (!ifaces.count(logEntryIntf)) continue;
            std::string svc;
            try
            {
                auto getMsg = dbus->new_method_call(
                    "xyz.openbmc_project.ObjectMapper",
                    "/xyz/openbmc_project/object_mapper",
                    "xyz.openbmc_project.ObjectMapper", "GetObject");
                getMsg.append(std::string(path), std::vector<std::string>{});
                auto getReply = dbus->call(getMsg);
                std::map<std::string, std::vector<std::string>> svcMap;
                getReply.read(svcMap);
                if (!svcMap.empty()) svc = svcMap.begin()->first;
            }
            catch (...) { svc = logSvc; }

            try
            {
                auto delMsg = dbus->new_method_call(
                    svc.c_str(), std::string(path).c_str(),
                    "xyz.openbmc_project.Object.Delete", "Delete");
                dbus->call_noreply(delMsg);
            }
            catch (...) {}
            ++deletedCount;
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ipmiStorageClearSEL: failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ClearSEL: complete",
        phosphor::logging::entry("DELETED=%u", deletedCount));
    return ipmi::responseSuccess(static_cast<uint8_t>(0xFF));
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

    // GetSELEntry (0x43) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x43),
                          ipmi::Privilege::User, ipmiStorageGetSELEntry);

    // AddSELEntry (0x44) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x44),
                          ipmi::Privilege::Operator, ipmiStorageAddSELEntry);

    // ClearSEL (0x47) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x47),
                          ipmi::Privilege::Operator, ipmiStorageClearSEL);

    // GetSELTime (0x48) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x48),
                          ipmi::Privilege::User, ipmiStorageGetSELTime);
}

} // namespace asrock
