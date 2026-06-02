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

// ── Command IDs (confirmed from live X570D4I-2T POST captures + Intel MDR-v2 spec) ──
//
// Intel MDR-v2 spec (NetFn 0x2E = netFnOemEight) defines:
//   0x30 AgentStatus  0x31 GetDir    0x32 GetDataInfo  0x33 LockData
//   0x35 GetDataBlock 0x38 SendDir   0x39 SendDataInfoOffer
//   0x3a SendDataInfo 0x3b DataStart 0x3c DataDone     0x3d SendDataBlock
//
// This BIOS uses the same command IDs but on NetFn 0x32 (AMI OEM) not 0x2E.
//
// Observed (confirmed):
//   0x72  GetBlock     – BIOS checks if BMC has cached SMBIOS (respond: no data)
//   0x3D  SendDataBlock – BIOS PUSHES a SMBIOS chunk to BMC  ← DATA IS HERE
//   0x5D  DataDone/SendDir – BIOS signals end of transfer or sends directory
//
// Intel MDR-v2 canonical (speculative on NetFn 0x32):
static constexpr ipmi::Cmd cmdMdrGetBlock    = 0x72;  // Poll: BMC has data?
static constexpr ipmi::Cmd cmdMdrSendBlock   = 0x3D;  // BIOS→BMC data push
static constexpr ipmi::Cmd cmdMdrSendDir     = 0x5D;  // BIOS dir / DataDone
// Standard Intel MDR-v2 (speculative, mirrored):
static constexpr ipmi::Cmd cmdMdrAgentStatus = 0x30;
static constexpr ipmi::Cmd cmdMdrGetDir      = 0x31;
static constexpr ipmi::Cmd cmdMdrDataStart   = 0x3B;
static constexpr ipmi::Cmd cmdMdrDataDone    = 0x3C;
// Legacy simple-sequence IDs also covered:
static constexpr ipmi::Cmd cmdMdrBeginLeg    = 0x20;
static constexpr ipmi::Cmd cmdMdrWriteLeg    = 0x21;
static constexpr ipmi::Cmd cmdMdrEndLeg      = 0x22;

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
// Cmd 0x72  — BIOS polls BMC for cached SMBIOS block
//
// Intel MDR-v2: GetDataBlock. Request contains region/block selectors.
// We respond with "no data" (completion code 0xCA = Out Of Space) so the
// BIOS knows it must push fresh tables.  We also hex-dump the request for
// protocol analysis.
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<>
handlerMdrGetBlock(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x72 GetBlock (poll)",
                     entry("REQ=%s", hexDump(req).c_str()));
    // Return "out of space / no data" so BIOS proceeds to push its tables.
    return ipmi::responseOutOfSpace();
}

// ────────────────────────────────────────────────────────────────────────────
// Cmd 0x3D  — Intel MDR-v2 SendDataBlock: BIOS PUSHES an SMBIOS chunk to BMC
//
// This is the primary data-transfer command.  The BIOS sends one or more of
// these with the raw SMBIOS table bytes as payload.  We accumulate them in
// g_smbiosBuf.  Session auto-begins on first chunk if no DataStart was seen.
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<>
handlerMdrSendBlock(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> chunk)
{
    if (g_state == MdrState::Idle)
    {
        log<level::INFO>("ami-ipmi-oem: Cmd 0x3D SendDataBlock — auto-begin session");
        g_smbiosBuf.clear();
        g_smbiosBuf.reserve(65536);
        g_expected = 0;
        g_state    = MdrState::Open;
    }

    log<level::INFO>("ami-ipmi-oem: Cmd 0x3D SendDataBlock",
                     entry("CHUNK=%zu", chunk.size()),
                     entry("TOTAL=%zu", g_smbiosBuf.size() + chunk.size()),
                     entry("FIRST16=%s", hexDump(
                         std::vector<uint8_t>(chunk.begin(),
                             chunk.begin() + std::min(chunk.size(), size_t{16}))).c_str()));

    if (chunk.empty())
        return ipmi::responseReqDataLenInvalid();

    g_smbiosBuf.insert(g_smbiosBuf.end(), chunk.begin(), chunk.end());
    g_state = MdrState::Receiving;
    return ipmi::responseSuccess();
}

// ────────────────────────────────────────────────────────────────────────────
// Cmd 0x5D  — DataDone / SendDir: BIOS signals end of transfer OR pushes dir
//
// Could be Intel MDR-v2 DataDone equivalent.  If we have buffered data,
// finalize it.  Also hex-dump the payload for analysis.
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<uint8_t>
handlerMdrSendDir(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x5D SendDir/DataDone",
                     entry("LEN=%zu", req.size()),
                     entry("REQ=%s", hexDump(req).c_str()),
                     entry("BUFSIZE=%zu", g_smbiosBuf.size()));

    if (g_state == MdrState::Receiving)
    {
        bool ok = writeSmbiosFile();
        if (ok)
            triggerMdrSync();
        g_state    = MdrState::Idle;
        g_expected = 0;
        return ok ? ipmi::responseSuccess(uint8_t{0x00})
                  : ipmi::responseUnspecifiedError();
    }

    // Not in Receiving state — just acknowledge so BIOS can continue.
    return ipmi::responseSuccess(uint8_t{0x00});
}

// ────────────────────────────────────────────────────────────────────────────
// Canonical Intel MDR-v2 AgentStatus (0x30) — BIOS handshake
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>
handlerMdrAgentStatus(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x30 AgentStatus",
                     entry("REQ=%s", hexDump(req).c_str()));
    return ipmi::responseSuccess(
        uint8_t{0x01},   // mdrVersion
        uint8_t{0x01},   // agentVersion
        uint8_t{0x00},   // dirVersion
        uint8_t{0x00},   // dirEntries
        uint8_t{0x01}    // dataRequest = 1 (BMC wants SMBIOS)
    );
}

// ────────────────────────────────────────────────────────────────────────────
// Canonical Intel MDR-v2 GetDir (0x31) — BIOS queries what BMC has stored
// ────────────────────────────────────────────────────────────────────────────
static ipmi::RspType<std::vector<uint8_t>>
handlerMdrGetDir(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: Cmd 0x31 GetDir",
                     entry("REQ=%s", hexDump(req).c_str()));

    std::vector<uint8_t> rsp;
    rsp.reserve(3 + 17);
    rsp.push_back(0x01);  // dirVersion
    rsp.push_back(0x01);  // 1 entry
    rsp.push_back(0x00);  // remaining
    // Entry: regionId=0, type=SMBIOS(0x01), ts=0, chk=0, validSz=0,
    //        maxSz=64K, updateCnt=0, xferType=0(full), updateRequired=1
    rsp.push_back(0x00); rsp.push_back(0x01);
    rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x01); rsp.push_back(0x00);
    rsp.push_back(0x00); rsp.push_back(0x00); rsp.push_back(0x01);
    return ipmi::responseSuccess(rsp);
}

// Generic begin/done handlers (for legacy cmd IDs and speculative coverage)
static ipmi::RspType<uint8_t>
handlerMdrDataBegin(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: DataBegin",
                     entry("REQ=%s", hexDump(req).c_str()));
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

static ipmi::RspType<uint8_t>
handlerMdrDataEnd(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>("ami-ipmi-oem: DataEnd",
                     entry("REQ=%s", hexDump(req).c_str()),
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
    log<level::INFO>("ami-ipmi-oem: registering AMI MDR handlers");

    // Helper lambda to register a handler on all three NetFns at once.
    auto reg = [](ipmi::NetFn nf, ipmi::Cmd cmd, auto fn) {
        ipmi::registerHandler(ipmi::prioOemBase, nf, cmd,
                              ipmi::Privilege::Admin, fn);
    };

    for (auto nf : {netFnAmi32, netFnAmi3A, netFnOem2E})
    {
        // ── Confirmed observed commands ────────────────────────────────────
        reg(nf, cmdMdrGetBlock,    handlerMdrGetBlock);    // 0x72 poll
        reg(nf, cmdMdrSendBlock,   handlerMdrSendBlock);   // 0x3D BIOS→BMC data
        reg(nf, cmdMdrSendDir,     handlerMdrSendDir);     // 0x5D done/dir

        // ── Canonical Intel MDR-v2 commands (speculative on these NetFns) ──
        reg(nf, cmdMdrAgentStatus, handlerMdrAgentStatus); // 0x30
        reg(nf, cmdMdrGetDir,      handlerMdrGetDir);      // 0x31
        reg(nf, cmdMdrDataStart,   handlerMdrDataBegin);   // 0x3B
        reg(nf, cmdMdrDataDone,    handlerMdrDataEnd);     // 0x3C

        // ── Legacy simple-sequence IDs ─────────────────────────────────────
        reg(nf, cmdMdrBeginLeg,    handlerMdrDataBegin);   // 0x20
        reg(nf, cmdMdrWriteLeg,    handlerMdrSendBlock);   // 0x21
        reg(nf, cmdMdrEndLeg,      handlerMdrDataEnd);     // 0x22
    }

    log<level::INFO>("ami-ipmi-oem: handlers registered");
}
