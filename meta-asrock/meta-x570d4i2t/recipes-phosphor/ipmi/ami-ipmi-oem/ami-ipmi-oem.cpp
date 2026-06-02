// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR V1 SMBIOS push handler for ASRock X570D4I-2T
//
// Uses the old-style C ipmi_register_callback() API to avoid crashes caused
// by the new-style template parameter-unpacking layer (SEGV on this ipmid version).
//
// Confirmed AMI MDR V1 protocol (NetFn 0x32 / 0x3A):
//
//   Step 1 — Handshake  (Cmd 0x3D  "Get MDR Status")
//     BIOS sends: [regionId, mdrVersion, sessionToken]   e.g. 01 00 07
//     BMC must return CC=0x00 + {status=0x00, maxSizeLSB=0x00,
//                                maxSizeMSB=0x10, mdrVersion=0x01}
//     → tells BIOS "ready, up to 4 KiB per chunk, MDR v1"
//
//   Step 2 — Lock  (Cmd 0x51  "MDR Write Begin")
//     BIOS sends: [regionId, totalSizeLow, totalSizeHigh, ...]
//     BMC: clears assembly buffer; reserves declared size
//     BMC returns: CC=0x00 (0 payload bytes)
//
//   Step 3 — Stream  (Cmd 0x52  "MDR Write Chunk", repeated)
//     BIOS sends: [regionId, offsetLow, offsetHigh, <SMBIOS bytes>]
//     BMC: strips 3-byte header, inserts payload at specified offset
//     BMC returns: CC=0x00 (0 payload bytes)
//
//   Step 4 — Commit  (Cmd 0x53  "MDR Write End")
//     BIOS sends: [regionId]
//     BMC: prepends MDRSMBIOSHeader, writes /var/lib/smbios/smbios2,
//          calls AgentSynchronizeData on smbios-mdrv2
//     BMC returns: CC=0x00 (0 payload bytes)
//
// NOTE: Do NOT use phosphor-logging/log.hpp here — the libphosphor_logging.so.1
// on this BMC (Megarac build) has an incompatible ABI. Calling log<>() results
// in a NULL function-pointer dereference (SIGSEGV). Use fprintf(stderr,...).

#include <ipmid/api.h>
#include <systemd/sd-bus.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#define LOG_INFO(fmt, ...)  fprintf(stderr, "ami-ipmi-oem [INFO]: " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "ami-ipmi-oem [WARN]: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   fprintf(stderr, "ami-ipmi-oem [ERR]:  " fmt "\n", ##__VA_ARGS__)

// ── NetFn constants ───────────────────────────────────────────────────────────
static constexpr uint8_t netFnAmi32 = 0x32;
static constexpr uint8_t netFnAmi3A = 0x3A;

// ── Command IDs (AMI MDR V1, confirmed from live POST captures) ───────────────
static constexpr uint8_t cmdMdrGetStatus  = 0x3D;  // Step 1: Handshake
static constexpr uint8_t cmdMdrWriteBegin = 0x51;  // Step 2: Lock/Open
static constexpr uint8_t cmdMdrWriteChunk = 0x52;  // Step 3: Data stream
static constexpr uint8_t cmdMdrWriteEnd   = 0x53;  // Step 4: Commit
static constexpr uint8_t cmdMdrGetBlock   = 0x72;  // Cache query (supplementary)
static constexpr uint8_t cmdMdrSendDir    = 0x5D;  // Legacy begin/end (supplementary)
static constexpr uint8_t cmdMdrAgentStatus = 0x30; // Agent handshake
static constexpr uint8_t cmdMdrGetDir     = 0x31;  // Directory query

// ── D-Bus / filesystem coordinates ───────────────────────────────────────────
static constexpr const char* kSmbiosDir  = "/var/lib/smbios";
static constexpr const char* kSmbiosFile = "/var/lib/smbios/smbios2";
static constexpr const char* kMdrService = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kMdrPath    = "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* kMdrIface   = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kSyncMethod = "AgentSynchronizeData";

// ── MDRSMBIOSHeader (OpenBMC smbios-mdrv2 expected layout) ───────────────────
struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;   // 0x02
    uint8_t  mdrType;      // 0x02 = SMBIOS
    uint32_t timestamp;    // epoch seconds (little-endian)
    uint32_t dataSize;     // payload bytes that follow (little-endian)
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10, "MDRSMBIOSHeader size mismatch");

// ── Assembly buffer state ─────────────────────────────────────────────────────
enum class MdrState : uint8_t { Idle, Open, Receiving };
static MdrState             g_state    = MdrState::Idle;
static uint32_t             g_expected = 0;  // bytes declared by Write Begin
static std::vector<uint8_t> g_smbiosBuf;

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string hexDump(const uint8_t* data, size_t len)
{
    std::ostringstream os;
    size_t limit = std::min(len, size_t{32});
    for (size_t i = 0; i < limit; ++i)
    {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        os << buf;
    }
    if (len > limit) os << "...";
    return os.str();
}

static bool writeSmbiosFile()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kSmbiosDir, ec);
    if (ec)
    {
        LOG_ERR("mkdir %s failed: %s", kSmbiosDir, ec.message().c_str());
        return false;
    }
    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        LOG_ERR("open %s for write failed", kSmbiosFile);
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
    LOG_INFO("smbios2 written: header(10) + data(%zu) = %zu bytes total",
             g_smbiosBuf.size(), sizeof(hdr) + g_smbiosBuf.size());
    return true;
}

static void triggerMdrSync()
{
    sd_bus* bus = ipmid_get_sd_bus_connection();
    if (!bus)
    {
        LOG_ERR("no sd-bus connection for AgentSynchronizeData");
        return;
    }
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, kMdrService, kMdrPath, kMdrIface,
                               kSyncMethod, &error, &reply, nullptr);
    if (r < 0)
        LOG_ERR("AgentSynchronizeData failed: %s",
                error.message ? error.message : "(no message)");
    else
        LOG_INFO("AgentSynchronizeData returned OK");
    sd_bus_error_free(&error);
    if (reply) sd_bus_message_unref(reply);
}

static void doCommit()
{
    if (g_smbiosBuf.size() < 4)
    {
        LOG_WARN("commit: buffer too small (%zu bytes) — discarding", g_smbiosBuf.size());
    }
    else
    {
        bool ok = writeSmbiosFile();
        if (ok) triggerMdrSync();
    }
    g_smbiosBuf.clear();
    g_state    = MdrState::Idle;
    g_expected = 0;
}

// ── Handler: Cmd 0x3D — Get MDR Status (Handshake) ───────────────────────────
static ipmi_ret_t handlerMdrGetStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x3D GetMdrStatus REQ[%zu]=%s state=%d buf=%zu",
             *data_len, hexDump(req, *data_len).c_str(),
             (int)g_state, g_smbiosBuf.size());

    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x00;  // Status: Unlocked / Ready
    rsp[1] = 0x00;  // Max Region Size LSB  } 0x1000 = 4096 bytes
    rsp[2] = 0x10;  // Max Region Size MSB  }
    rsp[3] = 0x01;  // MDR Version = 1
    
    // Handshake requires a 4-byte payload attached to the Completion Code
    *data_len = 4;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x51 — MDR Write Begin (Lock) ───────────────────────────────
static ipmi_ret_t handlerMdrWriteBegin(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x51 MdrWriteBegin REQ[%zu]=%s",
             *data_len, hexDump(req, *data_len).c_str());

    uint32_t declared = 0;
    if (*data_len >= 3)
        declared = static_cast<uint32_t>(req[1]) |
                   (static_cast<uint32_t>(req[2]) << 8);
    else if (*data_len >= 2)
        declared = req[1];

    g_smbiosBuf.clear();
    g_expected = declared;
    g_state    = MdrState::Open;
    LOG_INFO("MdrWriteBegin: session open, expecting %u bytes", declared);

    // CC Only. Sending extra payload bytes will crash the AMI BIOS state machine.
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x52 — MDR Write Chunk (Stream) ─────────────────────────────
static ipmi_ret_t handlerMdrWriteChunk(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    size_t len = *data_len;

    if (len <= 3)
    {
        LOG_WARN("Cmd 0x52 MdrWriteChunk: payload too short (%zu bytes)", len);
        *data_len = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }

    if (g_state == MdrState::Idle)
    {
        LOG_WARN("Cmd 0x52 MdrWriteChunk: auto-open (no Write Begin received)");
        g_smbiosBuf.clear();
        g_expected = 0;
        g_state    = MdrState::Open;
    }

    const uint8_t* payload = req + 3;       // skip [regionId, offsetLow, offsetHigh]
    size_t payloadLen = len - 3;
    uint16_t offset = static_cast<uint16_t>(req[1]) |
                      (static_cast<uint16_t>(req[2]) << 8);

    size_t requiredSize = offset + payloadLen;
    if (requiredSize > g_smbiosBuf.size())
    {
        // Sanity check: cap at 256KB to prevent KCS fuzzing / corruption from causing OOM
        if (requiredSize > 256 * 1024)
        {
            LOG_ERR("Cmd 0x52 MdrWriteChunk: offset/len too large (%zu bytes)", requiredSize);
            *data_len = 0;
            return IPMI_CC_REQ_DATA_LEN_INVALID;
        }
        g_smbiosBuf.resize(requiredSize, 0); // Zero-fill gap if chunks arrive out of order
    }

    // Safely copy payload to the precise offset
    std::copy(payload, payload + payloadLen, g_smbiosBuf.begin() + offset);
    
    g_state = MdrState::Receiving;
    LOG_INFO("Cmd 0x52 MdrWriteChunk: offset=0x%04X written=%zu total_cap=%zu",
             offset, payloadLen, g_smbiosBuf.size());

    // CC Only.
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x53 — MDR Write End (Commit) ───────────────────────────────
static ipmi_ret_t handlerMdrWriteEnd(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x53 MdrWriteEnd REQ[%zu]=%s total_buf=%zu",
             *data_len, hexDump(req, *data_len).c_str(), g_smbiosBuf.size());
             
    doCommit();
    
    // CC Only.
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x72 — GetBlock (BMC cache query) ───────────────────────────
static ipmi_ret_t handlerMdrGetBlock(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x72 GetBlock REQ[%zu]=%s state=%d buf=%zu",
             *data_len, hexDump(req, *data_len).c_str(),
             (int)g_state, g_smbiosBuf.size());
             
    // CC=0x00, 0 bytes → "no cache; proceed with full MDR transfer"
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x5D — Legacy MDR Begin/End (supplementary) ─────────────────
static ipmi_ret_t handlerMdrLegacyCtrl(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    uint8_t phase = (*data_len > 0) ? req[0] : 0;
    LOG_INFO("Cmd 0x5D LegacyCtrl phase=0x%02X REQ[%zu]=%s state=%d buf=%zu",
             phase, *data_len, hexDump(req, *data_len).c_str(),
             (int)g_state, g_smbiosBuf.size());

    *data_len = 0;

    if (phase == 0x01)
    {
        g_smbiosBuf.clear();
        g_expected = 0;
        g_state    = MdrState::Open;
        LOG_INFO("Cmd 0x5D phase=01: session opened (legacy Write Begin)");

        static_cast<uint8_t*>(response)[0] = 0x00;
        *data_len = 1;

    }
    else if (phase == 0x02)
    {
        LOG_INFO("Cmd 0x5D phase=02: committing %zu bytes (legacy Write End)",
                 g_smbiosBuf.size());
        doCommit();
    }

    // CC Only.
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x30 — Agent Status (handshake) ─────────────────────────────
static ipmi_ret_t handlerMdrAgentStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x30 AgentStatus REQ[%zu]=%s",
             *data_len, hexDump(req, *data_len).c_str());
             
    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;  // mdrVersion
    rsp[1] = 0x01;  // agentVersion
    rsp[2] = 0x00;  // dirVersion
    rsp[3] = 0x00;  // dirEntries
    rsp[4] = 0x01;  // dataRequest = 1 → BMC wants SMBIOS
    
    *data_len = 5;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x31 — Get MDR Directory ────────────────────────────────────
static ipmi_ret_t handlerMdrGetDir(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x31 GetDir REQ[%zu]=%s",
             *data_len, hexDump(req, *data_len).c_str());
             
    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;  // dirVersion
    rsp[1] = 0x01;  // 1 entry
    rsp[2] = 0x00;  // remaining
    
    static const uint8_t kEntry[] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01
    };
    memcpy(rsp + 3, kEntry, sizeof(kEntry));
    
    *data_len = 3 + sizeof(kEntry);
    return IPMI_CC_OK;
}

// ── Generic probe handler — logs unknown commands for protocol discovery ───────
static ipmi_ret_t handlerProbe(
    ipmi_netfn_t netfn, ipmi_cmd_t cmd,
    ipmi_request_t request, ipmi_response_t,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("PROBE NetFn=0x%02X Cmd=0x%02X REQ[%zu]=%s",
             netfn, cmd, *data_len, hexDump(req, *data_len).c_str());
    *data_len = 0;
    return IPMI_CC_INVALID_FIELD_REQUEST;
}

// ── Registration ──────────────────────────────────────────────────────────────
void setupGlobalOemFunctions() __attribute__((constructor));
void setupGlobalOemFunctions()
{
    LOG_INFO("registering AMI MDR handlers (protocol v2)");

    for (auto nf : {netFnAmi32, netFnAmi3A})
    {
        // Primary MDR V1 protocol handlers
        ipmi_register_callback(nf, cmdMdrGetStatus,  nullptr, handlerMdrGetStatus,  PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteBegin, nullptr, handlerMdrWriteBegin, PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteChunk, nullptr, handlerMdrWriteChunk, PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteEnd,   nullptr, handlerMdrWriteEnd,   PRIVILEGE_ADMIN);

        // Supplementary / legacy handlers
        ipmi_register_callback(nf, cmdMdrGetBlock,    nullptr, handlerMdrGetBlock,    PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrSendDir,     nullptr, handlerMdrLegacyCtrl,  PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrAgentStatus, nullptr, handlerMdrAgentStatus, PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrGetDir,      nullptr, handlerMdrGetDir,      PRIVILEGE_ADMIN);

        // Probe handlers
        static const uint8_t probeCmds[] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
            0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x3A, 0x3B, 0x3C, 0x3E, 0x3F,
            0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
            0x50, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F,
            0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
            0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
            0x70, 0x71, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
            0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
        };
        for (uint8_t c : probeCmds)
            ipmi_register_callback(nf, c, nullptr, handlerProbe, PRIVILEGE_ADMIN);
    }

    LOG_INFO("AMI MDR handlers registered");
}