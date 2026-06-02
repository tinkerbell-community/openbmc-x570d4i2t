// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR SMBIOS push handler for ASRock X570D4I-2T
//
// The AMI Aptio V UEFI BIOS pushes SMBIOS tables via a proprietary MDR
// (Managed Data Region) protocol on NetFn 0x32 over the KCS channel.
//
// ── Confirmed protocol (captured via busctl monitor during POST) ────────────
//
// NetFn 0x32 commands observed from the AMI BIOS:
//
//   Cmd 0x72  MdrIIGetDataBlock — BIOS polls BMC for a cached SMBIOS block
//             Request:  [regionId (1B), blkLow (1B), blkHigh (1B)]
//             Response: [status (1B)]  0x00 = no data; 0x01 = data follows
//             The BIOS issues ~10 polls per region (0x00 and 0x01) during
//             early POST to detect whether the BMC already has valid tables.
//
//   Cmd 0x3D  MdrIIAgentStatus — BIOS announces itself and queries BMC caps
//             Request:  [agentId (1B), dirVersion (1B), version (1B)]
//             Response: [mdrVersion (1B), agentVersion (1B), dirVersion (1B),
//                        dirEntries (1B), dataRequest (1B)]
//             dataRequest = 0x01 → BMC requests BIOS to send SMBIOS data.
//
//   Cmd 0x5D  MdrIIGetDirectory — BIOS queries what the BMC has cached
//             Request:  [dirIndex (1B)]
//             Response: [dirVersion (1B), dirEntries (1B), remaining (1B)]
//             When dirEntries=0 BIOS knows BMC has nothing; it will then push.
//
// After these three handshake commands succeed, the BIOS begins the data
// transfer using additional commands that are logged below as they arrive
// (check journald "ami-ipmi-oem" for NEW_CMD entries after the next POST).
//
// ── SMBIOS persistence ───────────────────────────────────────────────────────
// Once a full SMBIOS payload has been received and reassembled (via whatever
// data-write commands the BIOS uses), it is written to /var/lib/smbios/smbios2
// with a 10-byte MDRSMBIOSHeader, then xyz.openbmc_project.Smbios.MDR_V2 →
// AgentSynchronizeData is called to parse the tables into D-Bus inventory.

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace phosphor::logging;

// ── NetFn constants ──────────────────────────────────────────────────────────
static constexpr ipmi::NetFn netFnAmi32 = 0x32;  // AMI MDR (confirmed)
static constexpr ipmi::NetFn netFnAmi3A = 0x3A;  // AMI OEM (sensors/board)
static constexpr ipmi::NetFn netFnOem2E = 0x2E;  // Intel OEM (Group Extension)

// ── Confirmed command IDs (live D-Bus capture, X570D4I-2T POST) ──────────────
static constexpr ipmi::Cmd cmdMdrGetBlock   = 0x72;  // BIOS polls cached block
static constexpr ipmi::Cmd cmdMdrAgentStat  = 0x3D;  // BIOS announces itself
static constexpr ipmi::Cmd cmdMdrGetDir     = 0x5D;  // BIOS queries directory

// Data-write commands: not yet observed (BIOS aborted before reaching them).
// These are registered speculatively; their handlers log and accumulate data.
static constexpr ipmi::Cmd cmdMdrDataBegin  = 0x3B;  // ≈ send-region-info
static constexpr ipmi::Cmd cmdMdrDataWrite  = 0x3C;  // ≈ send-data-block
static constexpr ipmi::Cmd cmdMdrDataEnd    = 0x3E;  // ≈ transfer-done
// Legacy simple-sequence IDs also covered:
static constexpr ipmi::Cmd cmdMdrBeginLeg   = 0x20;
static constexpr ipmi::Cmd cmdMdrWriteLeg   = 0x21;
static constexpr ipmi::Cmd cmdMdrEndLeg     = 0x22;

// ── File-system / D-Bus coordinates ─────────────────────────────────────────
static constexpr const char* kSmbiosDir  = "/var/lib/smbios";
static constexpr const char* kSmbiosFile = "/var/lib/smbios/smbios2";
static constexpr const char* kMdrService = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kMdrPath    = "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* kMdrIface   = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kSyncMethod = "AgentSynchronizeData";

// ── MDRSMBIOSHeader — matches smbiosmdrv2app::readDataFromFlash() ────────────
struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;  // 0x02
    uint8_t  mdrType;     // 0x02 = SMBIOS region
    uint32_t timestamp;   // Unix epoch seconds, little-endian
    uint32_t dataSize;    // payload byte count immediately following
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10, "MDRSMBIOSHeader size mismatch");

// ── Transfer state machine ────────────────────────────────────────────────────
enum class MdrState : uint8_t { Idle, Open, Receiving };
static MdrState             g_state    = MdrState::Idle;
static uint32_t             g_expected = 0;
static std::vector<uint8_t> g_smbiosBuf;

// ── Hex dump helper (for logging unknown commands) ───────────────────────────
static std::string hexDump(const std::vector<uint8_t>& v)
{
    std::ostringstream os;
    for (auto b : v)
    {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", static_cast<unsigned>(b));
        os << buf;
    }
    return os.str();
}

// ── Persistence helpers ───────────────────────────────────────────────────────

static bool writeSmbiosFile()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kSmbiosDir, ec);
    if (ec)
    {
        log<level::ERR>("ami-ipmi-oem: mkdir smbios failed",
                        entry("ERR=%s", ec.message().c_str()));
        return false;
    }

    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        log<level::ERR>("ami-ipmi-oem: open smbios2 for write failed");
        return false;
    }

    MDRSMBIOSHeader hdr{};
    hdr.dirVersion = 0x02;
    hdr.mdrType    = 0x02;
    hdr.timestamp  = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    hdr.dataSize   = static_cast<uint32_t>(g_smbiosBuf.size());

    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    ofs.write(reinterpret_cast<const char*>(g_smbiosBuf.data()),
              static_cast<std::streamsize>(g_smbiosBuf.size()));

    log<level::INFO>("ami-ipmi-oem: smbios2 written",
                     entry("BYTES=%zu", g_smbiosBuf.size()));
    return true;
}

static void triggerMdrSync()
{
    auto bus = getSdBus();
    try
    {
        auto m = bus->new_method_call(kMdrService, kMdrPath,
                                      kMdrIface, kSyncMethod);
        auto r = bus->call(m);
        bool ok = false;
        r.read(ok);
        log<level::INFO>("ami-ipmi-oem: AgentSynchronizeData",
                         entry("OK=%d", static_cast<int>(ok)));
    }
    catch (const sdbusplus::exception_t& e)
    {
        log<level::ERR>("ami-ipmi-oem: AgentSynchronizeData failed",
                        entry("WHAT=%s", e.what()));
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Cmd 0x72  MdrIIGetDataBlock
//
// BIOS asks: "do you have a cached SMBIOS block for region R, block N?"
// We always answer "no data" so the BIOS knows it must push fresh tables.
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<uint8_t>
handlerMdrGetBlock(ipmi::Context::ptr /*ctx*/,
                   uint8_t regionId, uint8_t blkLow, uint8_t blkHigh)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x72 MdrGetBlock",
                     entry("REGION=0x%02X", regionId),
                     entry("BLOCK=%u", static_cast<unsigned>((blkHigh << 8) | blkLow)));
    // 0x00 = no cached block; BIOS will proceed to push data
    return ipmi::responseSuccess(uint8_t{0x00});
}

// ────────────────────────────────────────────────────────────────────────────
// Cmd 0x3D  MdrIIAgentStatus
//
// BIOS announces itself and asks for BMC's MDR capability.
// We respond: mdrVersion=1, agentVersion=1, dirVersion=0, entries=0,
// dataRequest=1 (bit 0 set = BMC wants BIOS to push SMBIOS data).
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>
handlerMdrAgentStatus(ipmi::Context::ptr /*ctx*/,
                      uint8_t agentId, uint8_t dirVer, uint8_t version)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x3D MdrAgentStatus",
                     entry("AGENT=0x%02X", agentId),
                     entry("DIRVER=0x%02X", dirVer),
                     entry("VER=0x%02X", version));
    return ipmi::responseSuccess(
        uint8_t{0x01},   // mdrVersion
        uint8_t{0x01},   // agentVersion
        uint8_t{0x00},   // dirVersion (0 = nothing cached)
        uint8_t{0x00},   // dirEntries (0 = BMC has no cached regions)
        uint8_t{0x01}    // dataRequest (1 = please send SMBIOS)
    );
}

// ────────────────────────────────────────────────────────────────────────────
// Cmd 0x5D  MdrIIGetDirectory
//
// BIOS queries which MDR regions the BMC supports/needs.
// We report 1 entry: SMBIOS region (type=0x01) with updateRequired=1 so the
// BIOS knows to proceed with the DataStart→DataBlock→DataDone transfer.
//
// Entry layout (17 bytes following the 3-byte header):
//   regionId(1) regionType(1) timestamp(4) dataChecksum(1) hdrChecksum(1)
//   validDataSize(4) maxDataSize(4) updateCount(1) xferType(1) dataSetHandle(1)
//   updateRequired(1) — some AMI stacks omit the last two fields; we include
//   the canonical 17-byte form.
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
handlerMdrGetDir(ipmi::Context::ptr /*ctx*/, uint8_t dirIndex)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x5D MdrGetDirectory",
                     entry("IDX=%u", dirIndex));

    std::vector<uint8_t> rsp;
    rsp.reserve(3 + 17);

    // Header: dirVersion, dirEntries, dataSetHandle / remaining
    rsp.push_back(0x01);  // dirVersion
    rsp.push_back(0x01);  // 1 entry
    rsp.push_back(0x00);  // remaining after this response

    // Entry 0: SMBIOS region descriptor
    rsp.push_back(0x00);  // regionId = 0
    rsp.push_back(0x01);  // regionType = 0x01 (SMBIOS)
    // timestamp (4 bytes LE) — 0 means "not valid / needs update"
    rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00);  // dataChecksum
    rsp.push_back(0x00);  // headerChecksum
    // validDataSize (4 bytes LE) = 0
    rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00);
    // maxDataSize (4 bytes LE) = 64 KiB
    rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x01); rsp.push_back(0x00);
    rsp.push_back(0x00);  // updateCount
    rsp.push_back(0x00);  // xferType = 0 (full copy)
    rsp.push_back(0x01);  // updateRequired = 1 → BIOS must push data

    return ipmi::responseSuccess(rsp);
}

// ────────────────────────────────────────────────────────────────────────────
// Data-transfer handlers (speculative — not yet observed)
//
// Registered for the command IDs most likely used by AMI BIOS for the actual
// SMBIOS block transfer that follows the handshake above.  They accumulate
// payload bytes and commit the file on End.
// ────────────────────────────────────────────────────────────────────────────

static ipmi::RspType<uint8_t>
handlerMdrDataBegin(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    std::string hex = hexDump(req);
    log<level::INFO>("ami-ipmi-oem: Cmd DataBegin received",
                     entry("LEN=%zu", req.size()),
                     entry("DATA=%s", hex.c_str()));

    uint32_t declared = 0;
    if (req.size() >= 4)
        std::memcpy(&declared, req.data(), 4);
    else if (!req.empty())
        declared = req[0];

    g_smbiosBuf.clear();
    g_smbiosBuf.reserve(declared ? declared : 65536);
    g_expected = declared;
    g_state    = MdrState::Open;

    log<level::INFO>("ami-ipmi-oem: MDR session opened",
                     entry("EXPECTED=%u", g_expected));
    return ipmi::responseSuccess(uint8_t{0x00});
}

static ipmi::RspType<>
handlerMdrDataWrite(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> chunk)
{
    log<level::INFO>("ami-ipmi-oem: Cmd DataWrite received",
                     entry("CHUNK=%zu", chunk.size()),
                     entry("TOTAL=%zu", g_smbiosBuf.size() + chunk.size()));

    // Auto-begin if the BIOS skipped the Begin phase (e.g. after AgentStatus
    // the BIOS goes straight to DataWrite without sending DataBegin first).
    if (g_state == MdrState::Idle)
    {
        log<level::INFO>("ami-ipmi-oem: auto-begin MDR session on first DataWrite");
        g_smbiosBuf.clear();
        g_smbiosBuf.reserve(65536);
        g_expected = 0;
        g_state    = MdrState::Open;
    }

    if (chunk.empty())
        return ipmi::responseReqDataLenInvalid();

    g_smbiosBuf.insert(g_smbiosBuf.end(), chunk.begin(), chunk.end());
    g_state = MdrState::Receiving;
    return ipmi::responseSuccess();
}

static ipmi::RspType<uint8_t>
handlerMdrDataEnd(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    std::string hex = hexDump(req);
    log<level::INFO>("ami-ipmi-oem: Cmd DataEnd received",
                     entry("TRAILING=%s", hex.c_str()),
                     entry("TOTAL=%zu", g_smbiosBuf.size()));

    if (g_state == MdrState::Idle)
    {
        log<level::WARNING>("ami-ipmi-oem: DataEnd with no active session");
        return ipmi::responseUnspecifiedError();
    }

    bool ok = writeSmbiosFile();
    if (ok)
        triggerMdrSync();

    g_state    = MdrState::Idle;
    g_expected = 0;

    return ok ? ipmi::responseSuccess(uint8_t{0x00})
              : ipmi::responseUnspecifiedError();
}

// ── Handler registration ─────────────────────────────────────────────────────
static void registerAmiIpmiOem() __attribute__((constructor));
static void registerAmiIpmiOem()
{
    log<level::INFO>(
        "ami-ipmi-oem: registering AMI MDR handlers",
        entry("NETFN_32=0x%02X", static_cast<unsigned>(netFnAmi32)),
        entry("NETFN_3A=0x%02X", static_cast<unsigned>(netFnAmi3A)));

    // ── NetFn 0x32 — AMI MDR (confirmed) ───────────────────────────────────
    // Handshake commands (confirmed observed):
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrGetBlock,
                          ipmi::Privilege::Admin, handlerMdrGetBlock);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrAgentStat,
                          ipmi::Privilege::Admin, handlerMdrAgentStatus);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrGetDir,
                          ipmi::Privilege::Admin, handlerMdrGetDir);

    // Data-transfer commands (speculative, not yet observed):
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrDataBegin,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrDataWrite,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrDataEnd,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrBeginLeg,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrWriteLeg,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi32, cmdMdrEndLeg,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);

    // ── NetFn 0x3A — AMI OEM (board name / sensors pushed by BIOS) ─────────
    // Handshake commands (mirror of 0x32 set):
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrGetBlock,
                          ipmi::Privilege::Admin, handlerMdrGetBlock);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrAgentStat,
                          ipmi::Privilege::Admin, handlerMdrAgentStatus);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrGetDir,
                          ipmi::Privilege::Admin, handlerMdrGetDir);

    // Data-transfer on 0x3A:
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrDataBegin,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrDataWrite,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrDataEnd,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrBeginLeg,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrWriteLeg,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmi3A, cmdMdrEndLeg,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);

    // ── NetFn 0x2E — Intel OEM / Group Extension ────────────────────────────
    // Some AMI BIOS versions send MDR control commands here.
    // Mirror all handlers so we catch whichever NetFn the BIOS actually uses.
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrGetBlock,
                          ipmi::Privilege::Admin, handlerMdrGetBlock);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrAgentStat,
                          ipmi::Privilege::Admin, handlerMdrAgentStatus);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrGetDir,
                          ipmi::Privilege::Admin, handlerMdrGetDir);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrDataBegin,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrDataWrite,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrDataEnd,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrBeginLeg,
                          ipmi::Privilege::Admin, handlerMdrDataBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrWriteLeg,
                          ipmi::Privilege::Admin, handlerMdrDataWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnOem2E, cmdMdrEndLeg,
                          ipmi::Privilege::Admin, handlerMdrDataEnd);

    log<level::INFO>("ami-ipmi-oem: handlers registered");
}
