// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR SMBIOS push handler for ASRock X570D4I-2T
//
// The AMI Aptio V UEFI BIOS pushes SMBIOS tables via a proprietary MDR
// (Managed Data Region) block-transfer protocol over the KCS channel.
// It targets OEM NetFns (0x3A primary, 0x32 alternate).  This provider
// intercepts those IPMI commands, reassembles the fragmented payload in
// a local buffer, prepends the MDRSMBIOSHeader expected by smbiosmdrv2app,
// writes the result to /var/lib/smbios/smbios2, and calls
// xyz.openbmc_project.Smbios.MDR_V2 → AgentSynchronizeData so the
// running daemon parses the new file and populates D-Bus inventory.
//
// ── Protocol discovery ──────────────────────────────────────────────────────
// The exact command IDs the AMI BIOS uses are logged at INFO level every
// time a handler fires.  After the first BIOS POST, run:
//   journalctl -u phosphor-ipmi-host -g "ami-ipmi-oem"
// to see which NetFn / Cmd pairs actually arrive and tune the constants
// cmdMdrBegin / cmdMdrWrite / cmdMdrEnd if needed.

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace phosphor::logging;

// ── NetFn constants ──────────────────────────────────────────────────────────
// Both are registered; boot logs will reveal which the BIOS actually uses.
static constexpr ipmi::NetFn netFnAmiMdr    = 0x3A;  // primary (OEM group)
static constexpr ipmi::NetFn netFnAmiMdrAlt = 0x32;  // alternate OEM group

// ── MDR protocol phase command IDs (AMI v1/v2 mixed set) ────────────────────
// Phase 1 — Open / Begin: BIOS declares upcoming transfer size
static constexpr ipmi::Cmd cmdMdrBegin     = 0x20;
// Phase 2 — Write Chunk: BIOS sends raw SMBIOS bytes, one block at a time
static constexpr ipmi::Cmd cmdMdrWrite     = 0x21;
// Phase 3 — End / Close: BIOS signals transfer complete
static constexpr ipmi::Cmd cmdMdrEnd       = 0x22;

// Intel MDR-v2-style alternative IDs sometimes seen on AMI platforms:
static constexpr ipmi::Cmd cmdMdrDataStart = 0x3B;   // ≈ Begin
static constexpr ipmi::Cmd cmdMdrDataBlock = 0x3D;   // ≈ Write
static constexpr ipmi::Cmd cmdMdrDataDone  = 0x3C;   // ≈ End

// ── File-system / D-Bus coordinates ─────────────────────────────────────────
static constexpr const char* kSmbiosDir   = "/var/lib/smbios";
static constexpr const char* kSmbiosFile  = "/var/lib/smbios/smbios2";
static constexpr const char* kMdrService  = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kMdrObjPath  = "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* kMdrIface    = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kSyncMethod  = "AgentSynchronizeData";

// ── MDRSMBIOSHeader ──────────────────────────────────────────────────────────
// Matches the struct expected by smbiosmdrv2app::readDataFromFlash().
// See: smbios-mdr/include/smbios_mdrv2.hpp
struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;  // 0x02
    uint8_t  mdrType;     // 0x02 = mdrTypeII (SMBIOS region)
    uint32_t timestamp;   // seconds since Unix epoch, little-endian
    uint32_t dataSize;    // raw SMBIOS byte count that immediately follows
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10,
              "MDRSMBIOSHeader must be exactly 10 bytes");

// ── Transfer-session state machine ───────────────────────────────────────────
enum class MdrState : uint8_t { Idle, Open, Receiving };

static MdrState             g_state    = MdrState::Idle;
static uint32_t             g_expected = 0;
static std::vector<uint8_t> g_smbiosBuf;

// ── Internal helpers ─────────────────────────────────────────────────────────

static bool writeSmbiosFile()
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(kSmbiosDir, ec);
    if (ec)
    {
        log<level::ERR>("ami-ipmi-oem: cannot create smbios directory",
                        entry("PATH=%s",  kSmbiosDir),
                        entry("ERROR=%s", ec.message().c_str()));
        return false;
    }

    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        log<level::ERR>("ami-ipmi-oem: failed to open smbios2 for writing",
                        entry("FILE=%s", kSmbiosFile));
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
    ofs.close();

    log<level::INFO>("ami-ipmi-oem: smbios2 written",
                     entry("FILE=%s",   kSmbiosFile),
                     entry("HEADER_BYTES=%zu", sizeof(hdr)),
                     entry("DATA_BYTES=%zu",   g_smbiosBuf.size()));
    return true;
}

static void triggerMdrSync()
{
    auto bus = getSdBus();
    try
    {
        auto method = bus->new_method_call(kMdrService, kMdrObjPath,
                                           kMdrIface, kSyncMethod);
        auto reply  = bus->call(method);
        bool ok     = false;
        reply.read(ok);
        log<level::INFO>("ami-ipmi-oem: AgentSynchronizeData returned",
                         entry("STATUS=%d", static_cast<int>(ok)));
    }
    catch (const sdbusplus::exception_t& e)
    {
        log<level::ERR>("ami-ipmi-oem: D-Bus AgentSynchronizeData failed",
                        entry("WHAT=%s", e.what()));
    }
}

// ── Phase 1: MDR Begin / Open ────────────────────────────────────────────────
//
// Expected request payload: 4-byte little-endian declared transfer size.
// Responds with a single 0x00 status byte on success.
static ipmi::RspType<uint8_t>
handlerMdrBegin(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> req)
{
    log<level::INFO>(
        "ami-ipmi-oem: MDR Begin received",
        entry("NETFN=0x%02X", static_cast<unsigned>(netFnAmiMdr)),
        entry("CMD=0x%02X",   static_cast<unsigned>(cmdMdrBegin)),
        entry("PAYLOAD_LEN=%zu", req.size()));

    if (req.size() < 4)
    {
        log<level::WARNING>(
            "ami-ipmi-oem: Begin payload too short – expected ≥4 bytes",
            entry("GOT=%zu", req.size()));
        return ipmi::responseReqDataLenInvalid();
    }

    uint32_t declared = 0;
    std::memcpy(&declared, req.data(), sizeof(declared));  // little-endian

    g_smbiosBuf.clear();
    g_smbiosBuf.reserve(declared);
    g_expected = declared;
    g_state    = MdrState::Open;

    log<level::INFO>("ami-ipmi-oem: MDR session opened",
                     entry("EXPECTED_BYTES=%u", g_expected));

    return ipmi::responseSuccess(uint8_t{0x00});
}

// ── Phase 2: MDR Write Chunk ─────────────────────────────────────────────────
//
// Request payload: raw SMBIOS bytes for this chunk (1 – N bytes).
static ipmi::RspType<>
handlerMdrWrite(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> chunk)
{
    log<level::INFO>(
        "ami-ipmi-oem: MDR Write received",
        entry("CMD=0x%02X",    static_cast<unsigned>(cmdMdrWrite)),
        entry("CHUNK_LEN=%zu", chunk.size()),
        entry("BUFFERED=%zu",  g_smbiosBuf.size()));

    if (g_state != MdrState::Open && g_state != MdrState::Receiving)
    {
        log<level::WARNING>(
            "ami-ipmi-oem: Write received outside of active MDR session");
        return ipmi::responseUnspecifiedError();
    }

    if (chunk.empty())
    {
        return ipmi::responseReqDataLenInvalid();
    }

    g_smbiosBuf.insert(g_smbiosBuf.end(), chunk.begin(), chunk.end());
    g_state = MdrState::Receiving;

    return ipmi::responseSuccess();
}

// ── Phase 3: MDR End / Close ─────────────────────────────────────────────────
//
// On success: writes header+buffer to /var/lib/smbios/smbios2 and
// triggers smbiosmdrv2app to parse it via D-Bus.
static ipmi::RspType<uint8_t>
handlerMdrEnd(ipmi::Context::ptr /*ctx*/, std::vector<uint8_t> /*req*/)
{
    log<level::INFO>(
        "ami-ipmi-oem: MDR End received",
        entry("CMD=0x%02X",      static_cast<unsigned>(cmdMdrEnd)),
        entry("TOTAL_BYTES=%zu", g_smbiosBuf.size()));

    if (g_state == MdrState::Idle)
    {
        log<level::WARNING>(
            "ami-ipmi-oem: End received with no active MDR session");
        return ipmi::responseUnspecifiedError();
    }

    if (!writeSmbiosFile())
    {
        g_state = MdrState::Idle;
        return ipmi::responseUnspecifiedError();
    }

    triggerMdrSync();

    g_state    = MdrState::Idle;
    g_expected = 0;
    // Intentionally keep g_smbiosBuf populated for post-mortem inspection;
    // it will be cleared on the next Begin.

    return ipmi::responseSuccess(uint8_t{0x00});
}

// ── Intel MDR-v2-style alternate command IDs (forwarded to same state machine)
// These log their own NetFn/Cmd so we can identify them in journald.

static ipmi::RspType<uint8_t>
handlerMdrDataStart(ipmi::Context::ptr ctx, std::vector<uint8_t> req)
{
    log<level::INFO>(
        "ami-ipmi-oem: DataStart (0x3B) aliased to Begin",
        entry("CMD=0x%02X", static_cast<unsigned>(cmdMdrDataStart)));
    return handlerMdrBegin(ctx, std::move(req));
}

static ipmi::RspType<>
handlerMdrDataBlock(ipmi::Context::ptr ctx, std::vector<uint8_t> req)
{
    log<level::INFO>(
        "ami-ipmi-oem: DataBlock (0x3D) aliased to Write",
        entry("CMD=0x%02X", static_cast<unsigned>(cmdMdrDataBlock)));
    return handlerMdrWrite(ctx, std::move(req));
}

static ipmi::RspType<uint8_t>
handlerMdrDataDone(ipmi::Context::ptr ctx, std::vector<uint8_t> req)
{
    log<level::INFO>(
        "ami-ipmi-oem: DataDone (0x3C) aliased to End",
        entry("CMD=0x%02X", static_cast<unsigned>(cmdMdrDataDone)));
    return handlerMdrEnd(ctx, std::move(req));
}

// ── Handler registration (called at library load time) ───────────────────────
static void registerAmiIpmiOem() __attribute__((constructor));
static void registerAmiIpmiOem()
{
    log<level::INFO>(
        "ami-ipmi-oem: registering AMI MDR handlers",
        entry("NETFN_PRIMARY=0x%02X",  static_cast<unsigned>(netFnAmiMdr)),
        entry("NETFN_ALTERNATE=0x%02X", static_cast<unsigned>(netFnAmiMdrAlt)));

    // ── Primary NetFn 0x3A ──────────────────────────────────────────────────
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrBegin,
                          ipmi::Privilege::Admin, handlerMdrBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrWrite,
                          ipmi::Privilege::Admin, handlerMdrWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrEnd,
                          ipmi::Privilege::Admin, handlerMdrEnd);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrDataStart,
                          ipmi::Privilege::Admin, handlerMdrDataStart);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrDataBlock,
                          ipmi::Privilege::Admin, handlerMdrDataBlock);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdr, cmdMdrDataDone,
                          ipmi::Privilege::Admin, handlerMdrDataDone);

    // ── Alternate NetFn 0x32 ────────────────────────────────────────────────
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrBegin,
                          ipmi::Privilege::Admin, handlerMdrBegin);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrWrite,
                          ipmi::Privilege::Admin, handlerMdrWrite);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrEnd,
                          ipmi::Privilege::Admin, handlerMdrEnd);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrDataStart,
                          ipmi::Privilege::Admin, handlerMdrDataStart);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrDataBlock,
                          ipmi::Privilege::Admin, handlerMdrDataBlock);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAmiMdrAlt, cmdMdrDataDone,
                          ipmi::Privilege::Admin, handlerMdrDataDone);

    log<level::INFO>("ami-ipmi-oem: all MDR handlers registered");
}
