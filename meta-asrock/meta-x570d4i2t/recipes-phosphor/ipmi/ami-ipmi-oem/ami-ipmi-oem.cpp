// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR SMBIOS push handler for ASRock X570D4I-2T
//
// Uses the old-style C ipmi_register_callback() API to avoid crashes caused
// by the new-style template parameter-unpacking layer (std::vector<uint8_t>
// deserialization SEGV on this ipmid version).
//
// Protocol (confirmed from live POST captures, NetFn 0x32):
//   Phase 1 — Directory announcement:
//     0x72  GetBlock REQ=00 01 00  (×N polls) — BIOS checks if BMC has cache;
//                                                CC=0x00 signals "no data, please push"
//     0x3D  SendBlock [01 00 07]              — BIOS pushes 3-byte MDR directory entry
//     0x5D  DataDone  REQ=[01]               — directory phase complete; BMC clears buffer
//   Phase 2 — Actual SMBIOS data:
//     0x72  GetBlock REQ=01 00 00            — BIOS asks "want actual data?"
//                                              CC=0x00 response triggers data push
//     0x3D  SendBlock [<chunk>]  (×N)       — BIOS pushes full SMBIOS tables
//     0x5D  DataDone  REQ=[02]              — data transfer done; BMC writes smbios2
//   0x30  AgentStatus — BIOS handshake; BMC replies "want SMBIOS" (dataRequest=1)
//   0x31  GetDir      — BIOS queries BMC directory
//
// NOTE: Do NOT use phosphor-logging/log.hpp here. The libphosphor_logging.so.1
// on the target BMC is from the original Megarac build and has an incompatible
// ABI with the version we compile against. Calling log<>() results in a NULL
// function pointer dereference (SIGSEGV) in ipmid. Use fprintf(stderr,...) instead.

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

// ── NetFn constants (confirmed) ───────────────────────────────────────────────
static constexpr uint8_t netFnAmi32 = 0x32;
static constexpr uint8_t netFnAmi3A = 0x3A;

// ── Command IDs ───────────────────────────────────────────────────────────────
static constexpr uint8_t cmdMdrGetBlock    = 0x72;
static constexpr uint8_t cmdMdrSendBlock   = 0x3D;
static constexpr uint8_t cmdMdrSendDir     = 0x5D;
static constexpr uint8_t cmdMdrAgentStatus = 0x30;
static constexpr uint8_t cmdMdrGetDir      = 0x31;
static constexpr uint8_t cmdMdrDataStart   = 0x3B;
static constexpr uint8_t cmdMdrDataDone    = 0x3C;
static constexpr uint8_t cmdMdrBeginLeg    = 0x20;
static constexpr uint8_t cmdMdrWriteLeg    = 0x21;
static constexpr uint8_t cmdMdrEndLeg      = 0x22;

// ── D-Bus / file coordinates ──────────────────────────────────────────────────
static constexpr const char* kSmbiosDir  = "/var/lib/smbios";
static constexpr const char* kSmbiosFile = "/var/lib/smbios/smbios2";
static constexpr const char* kMdrService = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kMdrPath    = "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* kMdrIface   = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kSyncMethod = "AgentSynchronizeData";

// ── MDRSMBIOSHeader ───────────────────────────────────────────────────────────
struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;
    uint8_t  mdrType;
    uint32_t timestamp;
    uint32_t dataSize;
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10, "MDRSMBIOSHeader size mismatch");

// ── Transfer state ────────────────────────────────────────────────────────────
enum class MdrState : uint8_t { Idle, Open, Receiving };
static MdrState             g_state    = MdrState::Idle;
static uint32_t             g_expected = 0;
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
        LOG_ERR("mkdir smbios failed: %s", ec.message().c_str());
        return false;
    }
    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        LOG_ERR("open smbios2 for write failed");
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
    LOG_INFO("smbios2 written: %zu bytes", g_smbiosBuf.size());
    return true;
}

static void triggerMdrSync()
{
    sd_bus* bus = ipmid_get_sd_bus_connection();
    if (!bus)
    {
        LOG_ERR("no sd-bus connection");
        return;
    }
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, kMdrService, kMdrPath, kMdrIface,
                               kSyncMethod, &error, &reply, nullptr);
    if (r < 0)
        LOG_ERR("AgentSynchronizeData failed: %s", error.message ? error.message : "?");
    else
        LOG_INFO("AgentSynchronizeData OK");
    sd_bus_error_free(&error);
    if (reply) sd_bus_message_unref(reply);
}

// ── Old-style C handlers ──────────────────────────────────────────────────────

static ipmi_ret_t handlerMdrGetBlock(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        LOG_INFO("Cmd 0x72 GetBlock REQ[%zu]=%s state=%d buf=%zu",
                 *data_len, dump.c_str(), (int)g_state, g_smbiosBuf.size());
    } catch (...) {}
    // Return CC=0x00 (success) with status byte 0x00 = "data not present, update needed".
    // Returning CC=0xC4 (OUT_OF_SPACE) told the BIOS "busy/full" — it stopped pushing.
    // CC=0x00 signals readiness; the BIOS then pushes directory (phase 1) or actual
    // SMBIOS data (phase 2) via 0x3D SendBlock + 0x5D DataDone.
    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x00;  // dataStatus = not present / needs update
    *data_len = 1;
    return IPMI_CC_OK;
}

static ipmi_ret_t handlerMdrSendBlock(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        const uint8_t* req = static_cast<const uint8_t*>(request);
        size_t len = *data_len;

        if (g_state == MdrState::Idle)
        {
            LOG_INFO("Cmd 0x3D SendBlock — auto-begin session");
            g_smbiosBuf.clear();
            g_smbiosBuf.reserve(65536);
            g_expected = 0;
            g_state    = MdrState::Open;
        }

        if (len == 0)
        {
            *data_len = 0;
            return IPMI_CC_REQ_DATA_LEN_INVALID;
        }

        std::string first = hexDump(req, len);
        LOG_INFO("Cmd 0x3D SendBlock chunk=%zu total=%zu first=%s",
                 len, g_smbiosBuf.size() + len, first.c_str());

        g_smbiosBuf.insert(g_smbiosBuf.end(), req, req + len);
        g_state = MdrState::Receiving;
    } catch (const std::exception& e) {
        LOG_ERR("SendBlock exception: %s", e.what());
        *data_len = 0;
        return IPMI_CC_UNSPECIFIED_ERROR;
    }
    *data_len = 0;
    return IPMI_CC_OK;
}

static ipmi_ret_t handlerMdrSendDir(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        const uint8_t* req = static_cast<const uint8_t*>(request);
        uint8_t phase = (*data_len > 0) ? req[0] : 0;
        std::string dump = hexDump(req, *data_len);
        LOG_INFO("Cmd 0x5D DataDone phase=0x%02X len=%zu buf=%zu req=%s",
                 phase, *data_len, g_smbiosBuf.size(), dump.c_str());

        if (phase == 0x01)
        {
            // Phase 1: directory announcement done.
            // The 3-byte packet was MDR directory metadata — not SMBIOS table data.
            // Discard it, reset state to Idle so the next 0x3D auto-begins a fresh
            // session for the actual SMBIOS data pushed in phase 2.
            LOG_INFO("Cmd 0x5D phase=01: directory done — discarding %zu dir bytes, "
                     "resetting for data phase", g_smbiosBuf.size());
            g_smbiosBuf.clear();
            g_state    = MdrState::Idle;
            g_expected = 0;
            static_cast<uint8_t*>(response)[0] = 0x00;
            *data_len = 1;
            return IPMI_CC_OK;
        }

        // Phase 2 (req=02) or legacy (req=00): actual data transfer done → commit.
        if (g_state == MdrState::Receiving)
        {
            if (g_smbiosBuf.size() < 64)
            {
                LOG_WARN("Cmd 0x5D phase=0x%02X: suspiciously small buffer (%zu bytes) "
                         "— discarding, not writing smbios2", phase, g_smbiosBuf.size());
                g_smbiosBuf.clear();
                g_state    = MdrState::Idle;
                g_expected = 0;
                static_cast<uint8_t*>(response)[0] = 0x00;
                *data_len = 1;
                return IPMI_CC_OK;
            }
            bool ok = writeSmbiosFile();
            if (ok) triggerMdrSync();
            g_state    = MdrState::Idle;
            g_expected = 0;
            static_cast<uint8_t*>(response)[0] = 0x00;
            *data_len = 1;
            return ok ? IPMI_CC_OK : IPMI_CC_UNSPECIFIED_ERROR;
        }
    } catch (const std::exception& e) {
        LOG_ERR("SendDir exception: %s", e.what());
        g_state = MdrState::Idle;
        *data_len = 0;
        return IPMI_CC_UNSPECIFIED_ERROR;
    }
    static_cast<uint8_t*>(response)[0] = 0x00;
    *data_len = 1;
    return IPMI_CC_OK;
}

static ipmi_ret_t handlerMdrAgentStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        LOG_INFO("Cmd 0x30 AgentStatus req=%s", dump.c_str());
    } catch (...) {}
    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;  // mdrVersion
    rsp[1] = 0x01;  // agentVersion
    rsp[2] = 0x00;  // dirVersion
    rsp[3] = 0x00;  // dirEntries
    rsp[4] = 0x01;  // dataRequest = 1 → BMC wants SMBIOS
    *data_len = 5;
    return IPMI_CC_OK;
}

static ipmi_ret_t handlerMdrGetDir(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        LOG_INFO("Cmd 0x31 GetDir req=%s", dump.c_str());
    } catch (...) {}
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

static ipmi_ret_t handlerMdrDataBegin(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        const uint8_t* req = static_cast<const uint8_t*>(request);
        std::string dump = hexDump(req, *data_len);
        LOG_INFO("DataBegin req=%s", dump.c_str());
        uint32_t declared = 0;
        if (*data_len >= 4) memcpy(&declared, req, 4);
        else if (*data_len > 0) declared = req[0];
        g_smbiosBuf.clear();
        g_smbiosBuf.reserve(declared ? declared : 65536);
        g_expected = declared;
        g_state    = MdrState::Open;
    } catch (...) {}
    static_cast<uint8_t*>(response)[0] = 0x00;
    *data_len = 1;
    return IPMI_CC_OK;
}

static ipmi_ret_t handlerMdrDataEnd(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        LOG_INFO("DataEnd total=%zu req=%s", g_smbiosBuf.size(), dump.c_str());
    } catch (...) {}
    if (g_state == MdrState::Idle)
    {
        LOG_WARN("DataEnd outside session");
        *data_len = 0;
        return IPMI_CC_UNSPECIFIED_ERROR;
    }
    bool ok = writeSmbiosFile();
    if (ok) triggerMdrSync();
    g_state    = MdrState::Idle;
    g_expected = 0;
    static_cast<uint8_t*>(response)[0] = 0x00;
    *data_len = 1;
    return ok ? IPMI_CC_OK : IPMI_CC_UNSPECIFIED_ERROR;
}

// ── Registration ──────────────────────────────────────────────────────────────

// Generic probe handler for commands we intercept purely for logging.
static ipmi_ret_t handlerProbe(
    ipmi_netfn_t netfn, ipmi_cmd_t cmd,
    ipmi_request_t request, ipmi_response_t,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        LOG_INFO("PROBE NetFn=0x%02X Cmd=0x%02X len=%zu data=%s",
                 netfn, cmd, *data_len, dump.c_str());
    } catch (...) {}
    *data_len = 0;
    return IPMI_CC_INVALID_FIELD_REQUEST;
}

void setupGlobalOemFunctions() __attribute__((constructor));
void setupGlobalOemFunctions()
{
    LOG_INFO("registering AMI MDR handlers");

    for (auto nf : {netFnAmi32, netFnAmi3A})
    {
        ipmi_register_callback(nf, cmdMdrGetBlock,    nullptr, handlerMdrGetBlock,    PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrSendBlock,   nullptr, handlerMdrSendBlock,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrSendDir,     nullptr, handlerMdrSendDir,     PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrAgentStatus, nullptr, handlerMdrAgentStatus, PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrGetDir,      nullptr, handlerMdrGetDir,      PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrDataStart,   nullptr, handlerMdrDataBegin,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrDataDone,    nullptr, handlerMdrDataEnd,     PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrBeginLeg,    nullptr, handlerMdrDataBegin,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteLeg,    nullptr, handlerMdrSendBlock,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrEndLeg,      nullptr, handlerMdrDataEnd,     PRIVILEGE_ADMIN);

        // Probe handlers for unidentified commands — log everything the BIOS sends
        // on these NetFns so we can discover the full AMI MDR protocol.
        static const uint8_t probeCmds[] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3E, 0x3F,
            0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
            0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F,
            0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
            0x70, 0x71, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
        };
        for (uint8_t c : probeCmds)
            ipmi_register_callback(nf, c, nullptr, handlerProbe, PRIVILEGE_ADMIN);
    }

    LOG_INFO("handlers registered");
}
