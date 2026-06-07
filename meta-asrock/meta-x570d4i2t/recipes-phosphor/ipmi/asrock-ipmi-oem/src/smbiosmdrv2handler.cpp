// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// MDR2 (Managed Data Region version 2) SMBIOS IPMI command handlers for
// the ASRock X570D4I-2T BMC.
//
// Architecture
// ------------
// The OpenBMC `smbiosmdrv2app` daemon owns the D-Bus interface
//   xyz.openbmc_project.Smbios.MDR_V2 @ /xyz/openbmc_project/Smbios/MDR_V2
// and maintains the in-memory SMBIOS directory structure.  This file
// provides the IPMI→D-Bus bridge that lets the host BIOS push SMBIOS
// tables to the BMC over the KCS/SMM channel.
//
// All 12 MDR2 commands are implemented, matching the command codes and
// NetFn (0x3E / netFnOemEight) used by intel-ipmi-oem so that any BIOS
// firmware built against the standard phosphor OOB MDR2 protocol works
// without modification.
//
// Transfer flow (host BIOS → BMC during POST)
// -------------------------------------------
// 1. AgentStatus      – BIOS checks BMC directory version; BMC signals
//                       whether it needs an update.
// 2. GetDir           – BIOS reads the current directory (data set IDs).
// 3. SendDir          – BIOS writes its directory to the BMC.
// 4. DataInfoOffer    – BMC offers a data-set ID slot.
// 5. GetDataInfo      – BIOS queries current data-set metadata.    ← this cmd
// 6. SendDataInfo     – BIOS declares the size/version/timestamp.
// 7. DataStart        – BIOS opens a write session; BMC maps shared memory.
// 8. SendDataBlock×N  – BIOS writes SMBIOS chunks via shared memory.
// 9. DataDone         – BIOS closes the session; BMC parses SMBIOS → D-Bus.
//
// Reference: intel-ipmi-oem/src/smbiosmdrv2handler.cpp

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus constants for the smbios-mdr service
// -----------------------------------------------------------------------

static constexpr const char* mdrv2Path =
    "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* mdrv2Interface =
    "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* dbusProperties =
    "org.freedesktop.DBus.Properties";

// Fixed agent ID for the BIOS SMBIOS agent (matches intel-ipmi-oem)
static constexpr uint16_t smbiosAgentId     = 0x0101;
static constexpr uint8_t  mdr2Version       = 2;
static constexpr uint8_t  smbiosAgentVersion = 1;
static constexpr size_t   dataInfoSize       = 16;

// Completion code used when a shared-memory checksum is invalid
static constexpr ipmi::Cc ccOemInvalidChecksum = 0x85;

// -----------------------------------------------------------------------
// Forward declaration
// -----------------------------------------------------------------------

static void registerMDR2Functions() __attribute__((constructor));

// -----------------------------------------------------------------------
// Helper: resolve the D-Bus service name for mdrv2
// -----------------------------------------------------------------------

static std::string getMdrv2Service()
{
    auto dbus = getSdBus();
    return ipmi::getService(*dbus, mdrv2Interface, mdrv2Path);
}

// -----------------------------------------------------------------------
// 1. AgentStatus (0x30) — BIOS queries BMC MDR2 agent / directory version
//
// Request:  agentId (uint16_t), dirVersion (uint8_t)
// Response: mdrVersion, agentVersion, dirVersion, dirEntries, dataRequest
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>
    mdr2AgentStatus(uint16_t agentId, uint8_t /*dirVersion*/)
{
    if (agentId != smbiosAgentId)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2AgentStatus: unknown agent id",
            phosphor::logging::entry("ID=0x%04x", agentId));
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Read DirectoryEntries property
    uint8_t dirEntries = 0;
    try
    {
        ipmi::Value v = ipmi::getDbusProperty(*dbus, service, mdrv2Path,
                                               mdrv2Interface,
                                               "DirectoryEntries");
        dirEntries = std::get<uint8_t>(v);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2AgentStatus: DirectoryEntries read failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseResponseError();
    }

    // Always request the directory – the BIOS will decide whether to send it
    static constexpr uint8_t dirDataRequested    = 1;
    static constexpr uint8_t dirDataNotRequested = 0;
    uint8_t dataRequest = (dirEntries == 0) ? dirDataRequested
                                            : dirDataNotRequested;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [1/9] AgentStatus",
        phosphor::logging::entry("DIR_ENTRIES=%u", dirEntries),
        phosphor::logging::entry("DATA_REQUESTED=%u", dataRequest));
    return ipmi::responseSuccess(mdr2Version, smbiosAgentVersion,
                                 static_cast<uint8_t>(0), dirEntries,
                                 dataRequest);
}

// -----------------------------------------------------------------------
// 2. GetDir (0x31) — BMC returns directory entries
//
// Request:  agentId (uint16_t), dirIndex (uint8_t)
// Response: vector<uint8_t>
// -----------------------------------------------------------------------

ipmi::RspType<std::vector<uint8_t>>
    mdr2GetDir(uint16_t agentId, uint8_t dirIndex)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    sdbusplus::message_t method = dbus->new_method_call(
        service.c_str(), mdrv2Path, mdrv2Interface, "GetDirectoryInformation");
    method.append(dirIndex);

    std::vector<uint8_t> dataOut;
    try
    {
        sdbusplus::message_t reply = dbus->call(method);
        reply.read(dataOut);
    }
    catch (const sdbusplus::exception_t& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2GetDir: GetDirectoryInformation failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseResponseError();
    }

    if (dataOut.empty())
    {
        return ipmi::responseResponseError();
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "MDR2 [2/9] GetDir",
        phosphor::logging::entry("DIR_INDEX=%u", dirIndex),
        phosphor::logging::entry("RESPONSE_BYTES=%zu", dataOut.size()));
    return ipmi::responseSuccess(dataOut);
}

// -----------------------------------------------------------------------
// 3. GetDataInfo (0x32) — BMC returns data-set metadata
//
// The BIOS calls this to check whether the BMC already has up-to-date
// SMBIOS data for a given data-set ID (16-byte identifier).  The BMC
// responds with the data-set's validity flag, current size, data version,
// and timestamp so the BIOS can decide whether to re-send.
//
// Request:  agentId (uint16_t), dataInfo[16] (vector<uint8_t>)
// Response: vector<uint8_t>
//   [0]    mdrVersion
//   [1-16] dataInfo (echo)
//   [17]   validFlag  (0=invalid, 1=valid, 2=locked)
//   [18-21] dataSetSize (uint32_t, big-endian as packed by smbios-mdr)
//   [22]   dataVersion
//   [23-26] timestamp (uint32_t, big-endian)
// -----------------------------------------------------------------------

ipmi::RspType<std::vector<uint8_t>>
    mdr2GetDataInfo(uint16_t agentId, std::vector<uint8_t> dataInfo)
{
    constexpr size_t reqDataInfoSize = 16;
    if (dataInfo.size() < reqDataInfoSize)
    {
        return ipmi::responseReqDataLenInvalid();
    }
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Step 1: resolve the 16-byte ID to a directory index
    int idIndex = -1;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface, "FindIdIndex");
        method.append(dataInfo);
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(idIndex);
        }
        catch (const sdbusplus::exception_t& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "mdr2GetDataInfo: FindIdIndex failed",
                phosphor::logging::entry("ERROR=%s", e.what()));
            return ipmi::responseParmOutOfRange();
        }
    }
    if (idIndex < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2GetDataInfo: invalid data ID index",
            phosphor::logging::entry("IDINDEX=%d", idIndex));
        return ipmi::responseParmOutOfRange();
    }

    // Step 2: call GetDataInformation with the resolved index
    std::vector<uint8_t> res;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface, "GetDataInformation");
        method.append(static_cast<uint8_t>(idIndex));
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(res);
        }
        catch (const sdbusplus::exception_t& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "mdr2GetDataInfo: GetDataInformation failed",
                phosphor::logging::entry("ERROR=%s", e.what()));
            return ipmi::responseResponseError();
        }
    }

    if (res.empty())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2GetDataInfo: empty response from GetDataInformation");
        return ipmi::responseResponseError();
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "MDR2 [3/9] GetDataInfo",
        phosphor::logging::entry("ID_INDEX=%d", idIndex));
    return ipmi::responseSuccess(res);
}

// -----------------------------------------------------------------------
// 4. DataInfoOffer (0x39) — BMC offers a free data-set slot to the BIOS
//
// Request:  agentId (uint16_t)
// Response: vector<uint8_t> (16-byte data set ID)
// -----------------------------------------------------------------------

ipmi::RspType<std::vector<uint8_t>>
    mdr2DataInfoOffer(uint16_t agentId)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    sdbusplus::message_t method = dbus->new_method_call(
        service.c_str(), mdrv2Path, mdrv2Interface, "GetDataOffer");

    std::vector<uint8_t> dataOut;
    try
    {
        sdbusplus::message_t reply = dbus->call(method);
        reply.read(dataOut);
    }
    catch (const sdbusplus::exception_t& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2DataInfoOffer: GetDataOffer failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseResponseError();
    }

    if (dataOut.size() != dataInfoSize)
    {
        return ipmi::responseUnspecifiedError();
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "MDR2 [4/9] DataInfoOffer: slot offered");
    return ipmi::responseSuccess(dataOut);
}

// -----------------------------------------------------------------------
// 5. SendDir (0x38) — BIOS sends directory metadata to the BMC
//
// Request:  agentId, dirVersion, dirIndex, returnedEntries,
//           remainingEntries, dataInfo[]
// Response: bool terminate
// -----------------------------------------------------------------------

ipmi::RspType<bool>
    mdr2SendDir(uint16_t agentId, uint8_t dirVersion, uint8_t dirIndex,
                uint8_t returnedEntries, uint8_t remainingEntries,
                std::vector<uint8_t> dataInfo)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }
    if ((static_cast<size_t>(returnedEntries) * dataInfoSize) !=
        dataInfo.size())
    {
        return ipmi::responseReqDataLenInvalid();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    sdbusplus::message_t method = dbus->new_method_call(
        service.c_str(), mdrv2Path, mdrv2Interface, "SendDirectoryInformation");
    method.append(dirVersion, dirIndex, returnedEntries, remainingEntries,
                  dataInfo);

    bool terminate = false;
    try
    {
        sdbusplus::message_t reply = dbus->call(method);
        reply.read(terminate);
    }
    catch (const sdbusplus::exception_t& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2SendDir: SendDirectoryInformation failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseResponseError();
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [5/9] SendDir",
        phosphor::logging::entry("DIR_VERSION=%u", dirVersion),
        phosphor::logging::entry("ENTRIES=%u", returnedEntries),
        phosphor::logging::entry("TERMINATE=%d", static_cast<int>(terminate)));
    return ipmi::responseSuccess(terminate);
}

// -----------------------------------------------------------------------
// 6. SendDataInfo (0x3A) — BIOS declares size/version/timestamp
//
// Request:  agentId, dataInfo[16], validFlag, dataLength, dataVersion,
//           timeStamp
// Response: bool entryChanged
// -----------------------------------------------------------------------

ipmi::RspType<bool>
    mdr2SendDataInfo(uint16_t agentId,
                     std::array<uint8_t, dataInfoSize> dataInfo,
                     uint8_t validFlag, uint32_t dataLength,
                     uint32_t dataVersion, uint32_t timeStamp)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Resolve data-set index
    std::vector<uint8_t> dataInfoVec(dataInfo.begin(), dataInfo.end());
    int idIndex = -1;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface, "FindIdIndex");
        method.append(dataInfoVec);
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(idIndex);
        }
        catch (const sdbusplus::exception_t& e)
        {
            return ipmi::responseParmOutOfRange();
        }
    }
    if (idIndex < 0)
    {
        return ipmi::responseParmOutOfRange();
    }

    sdbusplus::message_t method = dbus->new_method_call(
        service.c_str(), mdrv2Path, mdrv2Interface, "SendDataInformation");
    method.append(static_cast<uint8_t>(idIndex), validFlag, dataLength,
                  dataVersion, timeStamp);

    bool entryChanged = false;
    try
    {
        sdbusplus::message_t reply = dbus->call(method);
        reply.read(entryChanged);
    }
    catch (const sdbusplus::exception_t& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2SendDataInfo: SendDataInformation failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return ipmi::responseResponseError();
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [6/9] SendDataInfo",
        phosphor::logging::entry("DATA_LENGTH=%u", dataLength),
        phosphor::logging::entry("DATA_VERSION=%u", dataVersion),
        phosphor::logging::entry("ENTRY_CHANGED=%d",
                                  static_cast<int>(entryChanged)));
    return ipmi::responseSuccess(entryChanged);
}

// -----------------------------------------------------------------------
// 7. LockData (0x33) — BIOS acquires exclusive write lock
//
// Request:  agentId, dataInfo[16], timeout (uint16_t, ms)
// Response: mdr2Version, session, dataLength, xferAddress, xferLength
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint16_t, uint32_t, uint32_t, uint32_t>
    mdr2LockData(uint16_t agentId,
                 std::array<uint8_t, dataInfoSize> dataInfo,
                 uint16_t timeout)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Resolve index
    std::vector<uint8_t> dataInfoVec(dataInfo.begin(), dataInfo.end());
    int idIndex = -1;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface, "FindIdIndex");
        method.append(dataInfoVec);
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(idIndex);
        }
        catch (const sdbusplus::exception_t& e)
        {
            return ipmi::responseParmOutOfRange();
        }
    }
    if (idIndex < 0)
    {
        return ipmi::responseParmOutOfRange();
    }

    // SynchronizeDirectoryCommonData returns {dataSetSize, dataVersion, timestamp}
    // and sets up the shared memory lock timeout.
    std::vector<uint32_t> commonData;
    {
        sdbusplus::message_t method =
            dbus->new_method_call(service.c_str(), mdrv2Path, mdrv2Interface,
                                  "SynchronizeDirectoryCommonData");
        method.append(static_cast<uint8_t>(idIndex),
                      static_cast<uint32_t>(0));
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(commonData);
        }
        catch (const sdbusplus::exception_t& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "mdr2LockData: SynchronizeDirectoryCommonData failed",
                phosphor::logging::entry("ERROR=%s", e.what()));
            return ipmi::responseResponseError();
        }
    }

    if (commonData.size() < 3)
    {
        return ipmi::responseResponseError();
    }

    // Return a synthetic session handle and placeholder xfer addresses.
    // The actual shared-memory DMA address is platform-specific; the BIOS
    // writes via SendDataBlock which copies into the D-Bus service buffer.
    static uint16_t sessionHandle = 1;
    uint16_t session = sessionHandle++;
    if (sessionHandle == 0)
    {
        sessionHandle = 1;
    }

    // xferAddress and xferLength of 0 signal that the BIOS must use the
    // SendDataBlock IPMI path rather than direct memory writes.
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [7/9] LockData",
        phosphor::logging::entry("SESSION=%u", session),
        phosphor::logging::entry("DATA_SET_SIZE=%u", commonData[0]));
    return ipmi::responseSuccess(mdr2Version, session,
                                 commonData[0], // dataSetSize
                                 static_cast<uint32_t>(0), // xferAddress
                                 static_cast<uint32_t>(0)); // xferLength
}

// -----------------------------------------------------------------------
// 8. UnlockData (0x34) — BIOS releases the write lock
//
// Request:  agentId (uint16_t), lockHandle (uint16_t)
// Response: (none beyond CC)
// -----------------------------------------------------------------------

ipmi::RspType<>
    mdr2UnlockData(uint16_t agentId, uint16_t /*lockHandle*/)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }
    // Lock state is managed by smbios-mdr; no explicit unlock call
    // needed on the D-Bus side for the IPMI path.
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "MDR2 [8/9] UnlockData: lock released");
    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// 9. DataStart (0x3B) — BIOS opens a new SMBIOS transfer session
//
// Request:  agentId, dataInfo[16], dataLength, xferAddress, xferLength,
//           timeout
// Response: xferStartAck (uint8_t), session (uint16_t)
// -----------------------------------------------------------------------

ipmi::RspType<uint8_t, uint16_t>
    cmd_mdr2_data_start(uint16_t agentId,
                        std::array<uint8_t, dataInfoSize> dataInfo,
                        uint32_t dataLength,
                        uint32_t /*xferAddress*/,
                        uint32_t /*xferLength*/,
                        uint16_t /*timeout*/)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Resolve index
    std::vector<uint8_t> dataInfoVec(dataInfo.begin(), dataInfo.end());
    int idIndex = -1;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface, "FindIdIndex");
        method.append(dataInfoVec);
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(idIndex);
        }
        catch (const sdbusplus::exception_t& e)
        {
            return ipmi::responseParmOutOfRange();
        }
    }
    if (idIndex < 0)
    {
        return ipmi::responseParmOutOfRange();
    }

    // Inform the smbios-mdr service of the expected data size
    std::vector<uint32_t> commonData;
    {
        sdbusplus::message_t method =
            dbus->new_method_call(service.c_str(), mdrv2Path, mdrv2Interface,
                                  "SynchronizeDirectoryCommonData");
        method.append(static_cast<uint8_t>(idIndex), dataLength);
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(commonData);
        }
        catch (const sdbusplus::exception_t& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "cmd_mdr2_data_start: SynchronizeDirectoryCommonData failed",
                phosphor::logging::entry("ERROR=%s", e.what()));
            return ipmi::responseResponseError();
        }
    }

    static uint16_t sessionHandle = 1;
    uint16_t session = sessionHandle++;
    if (sessionHandle == 0)
    {
        sessionHandle = 1;
    }

    static constexpr uint8_t xferStartAck = 1;
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [9/9-prep] DataStart",
        phosphor::logging::entry("DATA_LENGTH=%u", dataLength),
        phosphor::logging::entry("SESSION=%u", session));
    return ipmi::responseSuccess(xferStartAck, session);
}

// -----------------------------------------------------------------------
// 10. SendDataBlock (0x3D) — BIOS writes one SMBIOS chunk via IPMI
//
// Because LockData/DataStart return xferAddress=0, the BIOS must use
// this command to transmit each chunk.  Chunks are accumulated in a
// session buffer (keyed by lockHandle) and flushed when DataDone is
// received.
//
// Request:  agentId, lockHandle, xferOffset, xferLength, checksum,
//           data[xferLength]
// Response: (none beyond CC)
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Persistent SMBIOS file — written by cmd_mdr2_data_done so that
// smbiosmdrv2app can parse the table when AgentSynchronizeData() fires.
//
// Layout matches phosphor-smbios-mdr MDRSMBIOSHeader (packed, 9 bytes):
//   dirVer    = mdrDirVersion = 1
//   mdrType   = mdrTypeII     = 2
//   timestamp = seconds-since-epoch at time of transfer
//   dataSize  = raw SMBIOS table byte count
// followed immediately by the raw SMBIOS binary.
// -----------------------------------------------------------------------

static constexpr uint8_t     smbiosDirVer   = 1;
static constexpr uint8_t     smbiosMdrType  = 2;
static constexpr const char* smbiosDataFile = "/var/lib/smbios/smbios2";

#pragma pack(push, 1)
struct SmbiosMdrFileHeader
{
    uint8_t  dirVer;
    uint8_t  mdrType;
    uint32_t timestamp;
    uint32_t dataSize;
};
#pragma pack(pop)

static bool writeSmbiosFile(const std::vector<uint8_t>& blob)
{
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(smbiosDataFile).parent_path(), ec);
    if (ec)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "writeSmbiosFile: mkdir failed",
            phosphor::logging::entry("ERROR=%s", ec.message().c_str()));
        return false;
    }

    SmbiosMdrFileHeader hdr{};
    hdr.dirVer    = smbiosDirVer;
    hdr.mdrType   = smbiosMdrType;
    hdr.timestamp = static_cast<uint32_t>(std::time(nullptr));
    hdr.dataSize  = static_cast<uint32_t>(blob.size());

    std::ofstream out(smbiosDataFile,
                      std::ios_base::binary | std::ios_base::trunc);
    if (!out.is_open())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "writeSmbiosFile: cannot open file for writing",
            phosphor::logging::entry("PATH=%s", smbiosDataFile));
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr),
              static_cast<std::streamsize>(sizeof(hdr)));
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    out.close();
    if (out.fail())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "writeSmbiosFile: write failed",
            phosphor::logging::entry("PATH=%s", smbiosDataFile));
        return false;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "writeSmbiosFile: SMBIOS table written",
        phosphor::logging::entry("PATH=%s", smbiosDataFile),
        phosphor::logging::entry("BYTES=%zu", blob.size()));
    return true;
}

// Per-session accumulation buffer (indexed by session handle).
// Only one active session is expected at a time.
static std::vector<uint8_t> g_sessionBuffer;
static uint16_t             g_activeSession = 0;

ipmi::RspType<>
    mdr2SendDataBlock(uint16_t agentId, uint16_t lockHandle,
                      uint32_t xferOffset, uint32_t xferLength,
                      uint32_t checksum,
                      std::vector<uint8_t> data)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }
    if (data.size() < xferLength)
    {
        return ipmi::responseReqDataLenInvalid();
    }

    // Verify CRC-32 (additive 32-bit sum, same as intel-ipmi-oem)
    uint32_t calcChecksum = 0;
    for (uint32_t i = 0; i < xferLength; ++i)
    {
        calcChecksum += data[i];
    }
    if (calcChecksum != checksum)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "mdr2SendDataBlock: checksum mismatch",
            phosphor::logging::entry("EXPECTED=0x%08x", checksum),
            phosphor::logging::entry("CALCULATED=0x%08x", calcChecksum));
        return ipmi::response(ccOemInvalidChecksum);
    }

    // Initialise or extend the accumulation buffer
    if (lockHandle != g_activeSession)
    {
        g_sessionBuffer.clear();
        g_activeSession = lockHandle;
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MDR2 SendDataBlock: new transfer session",
            phosphor::logging::entry("SESSION=%u", lockHandle));
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "MDR2 SendDataBlock",
        phosphor::logging::entry("SESSION=%u", lockHandle),
        phosphor::logging::entry("OFFSET=%u", xferOffset),
        phosphor::logging::entry("LENGTH=%u", xferLength));
    size_t needed = xferOffset + xferLength;
    if (g_sessionBuffer.size() < needed)
    {
        g_sessionBuffer.resize(needed, 0);
    }
    std::copy(data.begin(), data.begin() + xferLength,
              g_sessionBuffer.begin() + xferOffset);

    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// 11. GetDataBlock (0x35) — BMC returns a stored data block (read path)
//
// Request:  agentId, lockHandle, xferOffset, xferLength
// Response: xferLength, checksum, data[]
// -----------------------------------------------------------------------

ipmi::RspType<uint32_t, uint32_t, std::vector<uint8_t>>
    mdr2GetDataBlock(uint16_t agentId, uint16_t /*lockHandle*/,
                     uint32_t xferOffset, uint32_t xferLength)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }
    if (g_sessionBuffer.empty() ||
        xferOffset >= static_cast<uint32_t>(g_sessionBuffer.size()))
    {
        return ipmi::responseParmOutOfRange();
    }

    uint32_t available =
        static_cast<uint32_t>(g_sessionBuffer.size()) - xferOffset;
    uint32_t outSize = (xferLength > available) ? available : xferLength;

    std::vector<uint8_t> out(g_sessionBuffer.begin() + xferOffset,
                             g_sessionBuffer.begin() + xferOffset + outSize);

    uint32_t calcChecksum = 0;
    for (auto b : out)
    {
        calcChecksum += b;
    }

    return ipmi::responseSuccess(outSize, calcChecksum, out);
}

// -----------------------------------------------------------------------
// 12. DataDone (0x3C) — BIOS signals transfer complete
//
// The BMC writes the accumulated buffer to persistent storage and calls
// AgentSynchronizeData on the smbios-mdr D-Bus service, which triggers
// SMBIOS parsing and publishes the results to D-Bus inventory.
//
// Request:  agentId (uint16_t), lockHandle (uint16_t)
// Response: (none beyond CC)
// -----------------------------------------------------------------------

ipmi::RspType<>
    cmd_mdr2_data_done(uint16_t agentId, uint16_t lockHandle)
{
    if (agentId != smbiosAgentId)
    {
        return ipmi::responseParmOutOfRange();
    }

    if (g_sessionBuffer.empty())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "cmd_mdr2_data_done: no data in session buffer");
        return ipmi::responseResponseError();
    }

    std::string service = getMdrv2Service();
    auto dbus = getSdBus();

    // Write the accumulated buffer to /var/lib/smbios/smbios2 with the
    // MDRSMBIOSHeader prefix so that smbiosmdrv2app can parse it when
    // AgentSynchronizeData() fires (mirrors smbiosPushWriteFile in the
    // Redfish host-interface handler, ported here to the IPMI path per
    // the intel-ipmi-oem ipmi_to_redfish_hooks.cpp side-effect pattern).
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "MDR2 [9/9] DataDone: flushing buffer",
        phosphor::logging::entry("SESSION=%u", lockHandle),
        phosphor::logging::entry("BYTES=%zu", g_sessionBuffer.size()));
    if (!writeSmbiosFile(g_sessionBuffer))
    {
        return ipmi::responseResponseError();
    }

    bool status = false;
    {
        sdbusplus::message_t method = dbus->new_method_call(
            service.c_str(), mdrv2Path, mdrv2Interface,
            "AgentSynchronizeData");
        try
        {
            sdbusplus::message_t reply = dbus->call(method);
            reply.read(status);
        }
        catch (const sdbusplus::exception_t& e)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "cmd_mdr2_data_done: AgentSynchronizeData failed",
                phosphor::logging::entry("ERROR=%s", e.what()));
            return ipmi::responseResponseError();
        }
    }

    if (!status)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "cmd_mdr2_data_done: AgentSynchronizeData returned false");
        return ipmi::responseUnspecifiedError();
    }

    // Clear the accumulation buffer
    g_sessionBuffer.clear();
    g_activeSession = 0;

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "cmd_mdr2_data_done: SMBIOS transfer complete, inventory updated");

    return ipmi::responseSuccess();
}

// -----------------------------------------------------------------------
// Handler registration (NetFn 0x3E / netFnOemEight)
// -----------------------------------------------------------------------

static void registerMDR2Functions()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock MDR2 SMBIOS handlers registered");

    // <AgentStatus>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIAgentStatus),
        ipmi::Privilege::Operator, mdr2AgentStatus);

    // <GetDir>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIGetDir),
        ipmi::Privilege::Operator, mdr2GetDir);

    // <GetDataInfo>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIGetDataInfo),
        ipmi::Privilege::Operator, mdr2GetDataInfo);

    // <LockData>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIILockData),
        ipmi::Privilege::Operator, mdr2LockData);

    // <UnlockData>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIUnlockData),
        ipmi::Privilege::Operator, mdr2UnlockData);

    // <GetDataBlock>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIGetDataBlock),
        ipmi::Privilege::Operator, mdr2GetDataBlock);

    // <SendDir>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIISendDir),
        ipmi::Privilege::Operator, mdr2SendDir);

    // <SendDataInfoOffer>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIISendDataInfoOffer),
        ipmi::Privilege::Operator, mdr2DataInfoOffer);

    // <SendDataInfo>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIISendDataInfo),
        ipmi::Privilege::Operator, mdr2SendDataInfo);

    // <DataStart>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIDataStart),
        ipmi::Privilege::Operator, cmd_mdr2_data_start);

    // <DataDone>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIIDataDone),
        ipmi::Privilege::Operator, cmd_mdr2_data_done);

    // <SendDataBlock>
    ipmi::registerHandler(
        ipmi::prioOemBase, static_cast<ipmi::NetFn>(ipmi::netFnOemSix),
        static_cast<ipmi::Cmd>(mdr::cmdMdrIISendDataBlock),
        ipmi::Privilege::Operator, mdr2SendDataBlock);
}

} // namespace asrock
