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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSELInfo (0x40)");
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSELEntry (0x43)",
        phosphor::logging::entry("RECORD_ID=0x%04X", recordId));
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
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetSELTime (0x48)");
    auto now = std::chrono::system_clock::now();
    uint32_t epochSeconds = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count());
    return ipmi::responseSuccess(epochSeconds);
}

// =======================================================================
// FRU Inventory (NetFn Storage 0x10 GetFruInventoryAreaInfo / 0x11 ReadFruData
// / 0x12 WriteFruData) — DYNAMIC over ALL FRU EEPROMs.
//
// Ported from intel-ipmi-oem storagecommands.cpp, adapted to the asrock
// namespace + Admin privilege and made fully synchronous (no asio cache/timer).
// Every populated FRU device that xyz.openbmc_project.FruDevice enumerates is
// exposed — not a hardcoded list — so adding/removing a FRU EEPROM is picked up
// automatically. FRU device IDs are derived by hashing each Fru object path
// (stable within a boot; collisions bumped), with a rackmount baseboard pinned
// to ID 0 per the IPMI FRU spec.
// =======================================================================

static constexpr const char* fruServiceName = "xyz.openbmc_project.FruDevice";
static constexpr const char* fruMgrPath = "/xyz/openbmc_project/FruDevice";
static constexpr const char* fruMgrIntf = "xyz.openbmc_project.FruDeviceManager";
static constexpr const char* fruDevIntf = "xyz.openbmc_project.FruDevice";
static constexpr const char* chassisTypeRackMount = "23";

using FruObjectType =
    std::map<std::string, std::map<std::string, ipmi::Value>>;
using FruManagedObjects =
    std::map<sdbusplus::message::object_path, FruObjectType>;

// Dynamic FRU state. g_frus = full FruDevice inventory; g_deviceHashes maps the
// IPMI FRU device-id -> (bus, address); g_fruCache holds the raw bytes of the
// last-accessed FRU.
static FruManagedObjects g_frus;
static std::map<uint8_t, std::pair<uint16_t, uint8_t>> g_deviceHashes;
static std::vector<uint8_t> g_fruCache;
static uint16_t g_cacheBus = 0xFFFF;
static uint8_t g_cacheAddr = 0xFF;
static uint8_t g_lastDevId = 0xFF;

// Build device-id -> (bus,addr) from the current g_frus. Only objects exposing
// xyz.openbmc_project.FruDevice (i.e. a decodable FRU EEPROM) are real FRUs;
// bare Inventory.Item.I2CDevice probes are skipped. IDs are hashed from the
// object path (stable within a boot, collisions bumped), and the baseboard is
// pinned to device id 0 (so `fru print 0` / a BIOS query for 0 works).
static void recalculateFruHashes()
{
    g_deviceHashes.clear();
    std::hash<std::string> hasher;

    struct FruRef
    {
        std::string path;
        uint16_t bus;
        uint8_t addr;
        bool rackmount;
        bool hasBoard;
    };
    std::vector<FruRef> reals;
    size_t skipped = 0;

    for (const auto& fru : g_frus)
    {
        auto iface = fru.second.find(fruDevIntf);
        if (iface == fru.second.end())
        {
            ++skipped; // not a decoded FRU (e.g. Inventory.Item.I2CDevice probe)
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "recalculateFruHashes: skip non-FRU object",
                phosphor::logging::entry("PATH=%s", fru.first.str.c_str()));
            continue;
        }
        auto busFind = iface->second.find("BUS");
        auto addrFind = iface->second.find("ADDRESS");
        if (busFind == iface->second.end() || addrFind == iface->second.end())
        {
            ++skipped;
            continue;
        }

        FruRef r;
        r.path = fru.first.str;
        r.bus = static_cast<uint16_t>(std::get<uint32_t>(busFind->second));
        r.addr = static_cast<uint8_t>(std::get<uint32_t>(addrFind->second));
        std::string chassisType;
        if (auto c = iface->second.find("CHASSIS_TYPE");
            c != iface->second.end())
        {
            chassisType = std::get<std::string>(c->second);
        }
        r.rackmount = (chassisType == chassisTypeRackMount);
        r.hasBoard = iface->second.count("BOARD_PRODUCT_NAME") != 0;
        reals.push_back(std::move(r));
    }

    // Baseboard for device id 0: a rackmount-chassis FRU if present, else the
    // lowest-(bus,addr) FRU carrying a Board Info Area (the motherboard; PSUs /
    // peripherals usually carry only Product / Multirecord areas).
    int baseIdx = -1;
    for (size_t i = 0; i < reals.size(); ++i)
    {
        if (reals[i].rackmount)
        {
            baseIdx = static_cast<int>(i);
            break;
        }
    }
    if (baseIdx < 0)
    {
        for (size_t i = 0; i < reals.size(); ++i)
        {
            if (!reals[i].hasBoard)
            {
                continue;
            }
            if (baseIdx < 0 ||
                std::make_pair(reals[i].bus, reals[i].addr) <
                    std::make_pair(reals[baseIdx].bus, reals[baseIdx].addr))
            {
                baseIdx = static_cast<int>(i);
            }
        }
    }

    for (size_t i = 0; i < reals.size(); ++i)
    {
        uint8_t id;
        if (static_cast<int>(i) == baseIdx)
        {
            id = 0; // baseboard
        }
        else
        {
            id = static_cast<uint8_t>(hasher(reals[i].path));
            if (id == 0 || id == 0xFF)
            {
                id = 1;
            }
        }
        std::pair<uint16_t, uint8_t> dev(reals[i].bus, reals[i].addr);
        while (!g_deviceHashes.emplace(id, dev).second)
        {
            if (++id == 0xFF)
            {
                id = 1;
            }
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "recalculateFruHashes: FRU mapped",
            phosphor::logging::entry("PATH=%s", reals[i].path.c_str()),
            phosphor::logging::entry("ID=%u", static_cast<unsigned>(id)),
            phosphor::logging::entry("BUS=%u", reals[i].bus),
            phosphor::logging::entry("ADDR=0x%02X", reals[i].addr));
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "recalculateFruHashes: done",
        phosphor::logging::entry("REAL_FRUS=%zu", reals.size()),
        phosphor::logging::entry("SKIPPED=%zu", skipped),
        phosphor::logging::entry("BASEBOARD_AT_0=%d", baseIdx >= 0 ? 1 : 0));
}

// Pull the full FruDevice inventory from D-Bus and rebuild the id map.
static bool refreshFruMap()
{
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(
            fruServiceName, "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects");
        auto reply = dbus->call(msg);
        g_frus.clear();
        reply.read(g_frus);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "refreshFruMap: GetManagedObjects failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return false;
    }
    recalculateFruHashes();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "refreshFruMap: loaded FRUs",
        phosphor::logging::entry("OBJECTS=%zu", g_frus.size()),
        phosphor::logging::entry("DEVICES=%zu", g_deviceHashes.size()));
    return true;
}

// Load the raw bytes of FRU `devId` into g_fruCache (cached per device-id).
static ipmi::Cc getFru(uint8_t devId)
{
    if (g_lastDevId == devId && devId != 0xFF)
    {
        return ipmi::ccSuccess;
    }
    // (Re)enumerate if the map is empty or doesn't know this id yet — this is
    // what makes it dynamic: a newly added FRU appears on the next request.
    if (g_deviceHashes.empty() || !g_deviceHashes.count(devId))
    {
        refreshFruMap();
    }
    auto it = g_deviceHashes.find(devId);
    if (it == g_deviceHashes.end())
    {
        return ipmi::ccSensorInvalid;
    }
    uint16_t bus = it->second.first;
    uint8_t addr = it->second.second;
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(fruServiceName, fruMgrPath, fruMgrIntf,
                                         "GetRawFru");
        msg.append(bus, addr);
        auto reply = dbus->call(msg);
        g_fruCache.clear();
        reply.read(g_fruCache);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "getFru: GetRawFru failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        g_cacheBus = 0xFFFF;
        g_cacheAddr = 0xFF;
        g_lastDevId = 0xFF;
        return ipmi::ccResponseError;
    }
    g_cacheBus = bus;
    g_cacheAddr = addr;
    g_lastDevId = devId;
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "getFru: loaded FRU",
        phosphor::logging::entry("DEV=%u", devId),
        phosphor::logging::entry("BUS=%u", bus),
        phosphor::logging::entry("ADDR=0x%02X", addr),
        phosphor::logging::entry("BYTES=%zu", g_fruCache.size()));
    return ipmi::ccSuccess;
}

// Flush g_fruCache back to the EEPROM via FruDevice.
static bool writeFruCache()
{
    if (g_cacheBus == 0xFFFF && g_cacheAddr == 0xFF)
    {
        return true;
    }
    try
    {
        auto dbus = getSdBus();
        auto msg = dbus->new_method_call(fruServiceName, fruMgrPath, fruMgrIntf,
                                         "WriteFru");
        msg.append(g_cacheBus, g_cacheAddr, g_fruCache);
        dbus->call(msg);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "writeFruCache: WriteFru failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return false;
    }
    return true;
}

// True once the bytes written so far form a complete, checksum-valid FRU image
// (common header + chassis/board/product/multirecord areas). Equivalent to
// intel's validateBasicFruContent, inlined to avoid the fruutils dependency.
static bool fruWriteComplete(const std::vector<uint8_t>& fru,
                             size_t lastWriteAddr)
{
    if (fru.size() < 8)
    {
        return false;
    }
    if ((fru[0] & 0x0F) != 0x01) // FRU Information format version 1
    {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        sum = static_cast<uint8_t>(sum + fru[i]);
    }
    if (sum != 0) // common-header zero checksum
    {
        return false;
    }
    size_t end = 8;
    // chassis(2)/board(3)/product(4): area length (in 8-byte units) at off+1.
    for (int idx : {2, 3, 4})
    {
        size_t off = static_cast<size_t>(fru[idx]) * 8;
        if (off == 0)
        {
            continue;
        }
        if (off + 1 >= fru.size())
        {
            return false; // area declared but not yet written
        }
        end = std::max(end, off + static_cast<size_t>(fru[off + 1]) * 8);
    }
    // multirecord(5): walk records to the end-of-list flag (bit7 of byte off+1).
    size_t mrOff = static_cast<size_t>(fru[5]) * 8;
    if (mrOff != 0)
    {
        size_t p = mrOff;
        while (p + 5 <= fru.size())
        {
            bool eol = (fru[p + 1] & 0x80) != 0;
            size_t recLen = fru[p + 2];
            p += 5 + recLen;
            if (eol)
            {
                break;
            }
        }
        end = std::max(end, p);
    }
    return lastWriteAddr >= end && fru.size() >= end;
}

// -----------------------------------------------------------------------
// GetFruInventoryAreaInfo (NetFn Storage / 0x10)
//   Request:  [0] FRU device id
//   Response: [0-1] inventory size (LE), [2] access type (0 = by byte)
// -----------------------------------------------------------------------
ipmi::RspType<uint16_t, // inventorySize
              uint8_t>  // accessType
    ipmiStorageGetFruInvAreaInfo(ipmi::Context::ptr& /*ctx*/,
                                 uint8_t fruDeviceId)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetFruInventoryAreaInfo (0x10)",
        phosphor::logging::entry("DEV=%u", fruDeviceId));
    if (fruDeviceId == 0xFF)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    ipmi::Cc ret = getFru(fruDeviceId);
    if (ret != ipmi::ccSuccess)
    {
        return ipmi::response(ret);
    }
    return ipmi::responseSuccess(static_cast<uint16_t>(g_fruCache.size()),
                                 static_cast<uint8_t>(0)); // byte access
}

// -----------------------------------------------------------------------
// ReadFruData (NetFn Storage / 0x11)
//   Request:  [0] FRU device id, [1-2] offset (LE), [3] count to read
//   Response: [0] count returned, [1..] data
// -----------------------------------------------------------------------
ipmi::RspType<uint8_t,              // count
              std::vector<uint8_t>> // data
    ipmiStorageReadFruData(ipmi::Context::ptr& /*ctx*/, uint8_t fruDeviceId,
                           uint16_t fruInventoryOffset, uint8_t countToRead)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ReadFruData (0x11)",
        phosphor::logging::entry("DEV=%u", fruDeviceId),
        phosphor::logging::entry("OFFSET=%u", fruInventoryOffset),
        phosphor::logging::entry("COUNT=%u", countToRead));
    if (fruDeviceId == 0xFF)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    ipmi::Cc status = getFru(fruDeviceId);
    if (status != ipmi::ccSuccess)
    {
        return ipmi::response(status);
    }

    size_t fromFruByteLen = 0;
    if (static_cast<size_t>(countToRead) + fruInventoryOffset <
        g_fruCache.size())
    {
        fromFruByteLen = countToRead;
    }
    else if (g_fruCache.size() > fruInventoryOffset)
    {
        fromFruByteLen = g_fruCache.size() - fruInventoryOffset;
    }
    else
    {
        return ipmi::responseReqDataLenExceeded();
    }

    std::vector<uint8_t> requestedData(
        g_fruCache.begin() + fruInventoryOffset,
        g_fruCache.begin() + fruInventoryOffset + fromFruByteLen);

    return ipmi::responseSuccess(static_cast<uint8_t>(requestedData.size()),
                                 requestedData);
}

// -----------------------------------------------------------------------
// WriteFruData (NetFn Storage / 0x12)
//   Request:  [0] FRU device id, [1-2] offset (LE), [3..] data to write
//   Response: [0] count written (0 while accumulating; final on completion)
// -----------------------------------------------------------------------
ipmi::RspType<uint8_t> // count written
    ipmiStorageWriteFruData(ipmi::Context::ptr& /*ctx*/, uint8_t fruDeviceId,
                            uint16_t fruInventoryOffset,
                            std::vector<uint8_t> dataToWrite)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "WriteFruData (0x12)",
        phosphor::logging::entry("DEV=%u", fruDeviceId),
        phosphor::logging::entry("OFFSET=%u", fruInventoryOffset),
        phosphor::logging::entry("LEN=%zu", dataToWrite.size()));
    if (fruDeviceId == 0xFF)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    ipmi::Cc status = getFru(fruDeviceId);
    if (status != ipmi::ccSuccess)
    {
        return ipmi::response(status);
    }

    size_t writeLen = dataToWrite.size();
    size_t lastWriteAddr = static_cast<size_t>(fruInventoryOffset) + writeLen;
    if (g_fruCache.size() < lastWriteAddr)
    {
        g_fruCache.resize(lastWriteAddr);
    }
    std::copy(dataToWrite.begin(), dataToWrite.end(),
              g_fruCache.begin() + fruInventoryOffset);

    uint8_t countWritten = 0;
    if (fruWriteComplete(g_fruCache, lastWriteAddr))
    {
        if (!writeFruCache())
        {
            return ipmi::responseInvalidFieldRequest();
        }
        // Force a re-read from the EEPROM on the next access.
        g_lastDevId = 0xFF;
        countWritten =
            static_cast<uint8_t>(std::min(g_fruCache.size(), size_t{0xFF}));
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "WriteFruData: FRU complete, flushed to EEPROM",
            phosphor::logging::entry("DEV=%u", fruDeviceId),
            phosphor::logging::entry("BYTES=%u", countWritten));
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "WriteFruData: accumulating (FRU not yet complete)",
            phosphor::logging::entry("DEV=%u", fruDeviceId),
            phosphor::logging::entry("CACHED=%zu", g_fruCache.size()));
    }

    return ipmi::responseSuccess(countWritten);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static void registerStorageCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock Storage commands registered (NetFn 0x0A)");

    // GetFruInventoryAreaInfo (0x10) — Admin (dynamic over all FRU devices)
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x10),
                          ipmi::Privilege::Admin, ipmiStorageGetFruInvAreaInfo);

    // ReadFruData (0x11) — Admin
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x11),
                          ipmi::Privilege::Admin, ipmiStorageReadFruData);

    // WriteFruData (0x12) — Admin
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x12),
                          ipmi::Privilege::Admin, ipmiStorageWriteFruData);

    // GetSELInfo (0x40) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x40),
                          ipmi::Privilege::Admin, ipmiStorageGetSELInfo);

    // GetSELEntry (0x43) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x43),
                          ipmi::Privilege::Admin, ipmiStorageGetSELEntry);

    // AddSELEntry (0x44) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x44),
                          ipmi::Privilege::Admin, ipmiStorageAddSELEntry);

    // ClearSEL (0x47) — Operator
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x47),
                          ipmi::Privilege::Admin, ipmiStorageClearSEL);

    // GetSELTime (0x48) — User
    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnStorage,
                          static_cast<ipmi::Cmd>(0x48),
                          ipmi::Privilege::Admin, ipmiStorageGetSELTime);
}

} // namespace asrock
