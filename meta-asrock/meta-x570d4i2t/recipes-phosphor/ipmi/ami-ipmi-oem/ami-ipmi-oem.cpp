// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR SMBIOS push handler for ASRock X570D4I-2T
//
// Uses the old-style C ipmi_register_callback() API to avoid crashes caused
// by the new-style template parameter-unpacking layer (std::vector<uint8_t>
// deserialization SEGV on this ipmid version).
//
// Protocol (confirmed from live POST captures, NetFn 0x32):
//   0x72  GetBlock    — BIOS polls BMC for cached SMBIOS; reply OutOfSpace→push
//   0x3D  SendBlock   — BIOS PUSHES raw SMBIOS chunk bytes to BMC
//   0x5D  DataDone    — BIOS signals end of transfer; BMC writes + notifies
//   0x30  AgentStatus — BIOS handshake; BMC replies "want SMBIOS"
//   0x31  GetDir      — BIOS queries BMC directory

#include <ipmid/api.h>
#include <phosphor-logging/log.hpp>
#include <systemd/sd-bus.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace phosphor::logging;

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
    sd_bus* bus = ipmid_get_sd_bus_connection();
    if (!bus)
    {
        log<level::ERR>("ami-ipmi-oem: no sd-bus connection");
        return;
    }
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, kMdrService, kMdrPath, kMdrIface,
                               kSyncMethod, &error, &reply, nullptr);
    if (r < 0)
        log<level::ERR>("ami-ipmi-oem: AgentSynchronizeData failed",
                        entry("ERR=%s", error.message ? error.message : "?"));
    else
        log<level::INFO>("ami-ipmi-oem: AgentSynchronizeData OK");
    sd_bus_error_free(&error);
    if (reply) sd_bus_message_unref(reply);
}

// ── Old-style C handlers ──────────────────────────────────────────────────────

static ipmi_ret_t handlerMdrGetBlock(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    try {
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        log<level::INFO>("ami-ipmi-oem: Cmd 0x72 GetBlock",
                         entry("REQ=%s", dump.c_str()));
    } catch (...) {}
    *data_len = 0;
    return IPMI_CC_OUT_OF_SPACE;  // no cached data → BIOS will push
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
            log<level::INFO>("ami-ipmi-oem: Cmd 0x3D SendBlock — auto-begin session");
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
        log<level::INFO>("ami-ipmi-oem: Cmd 0x3D SendBlock",
                         entry("CHUNK=%zu TOTAL=%zu FIRST=%s",
                               len, g_smbiosBuf.size() + len, first.c_str()));

        g_smbiosBuf.insert(g_smbiosBuf.end(), req, req + len);
        g_state = MdrState::Receiving;
    } catch (const std::exception& e) {
        log<level::ERR>("ami-ipmi-oem: SendBlock exception",
                        entry("ERR=%s", e.what()));
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
        std::string dump = hexDump(static_cast<const uint8_t*>(request), *data_len);
        log<level::INFO>("ami-ipmi-oem: Cmd 0x5D DataDone",
                         entry("LEN=%zu BUF=%zu REQ=%s",
                               *data_len, g_smbiosBuf.size(), dump.c_str()));

        if (g_state == MdrState::Receiving)
        {
            bool ok = writeSmbiosFile();
            if (ok) triggerMdrSync();
            g_state    = MdrState::Idle;
            g_expected = 0;
            static_cast<uint8_t*>(response)[0] = 0x00;
            *data_len = 1;
            return ok ? IPMI_CC_OK : IPMI_CC_UNSPECIFIED_ERROR;
        }
    } catch (const std::exception& e) {
        log<level::ERR>("ami-ipmi-oem: SendDir exception",
                        entry("ERR=%s", e.what()));
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
        log<level::INFO>("ami-ipmi-oem: Cmd 0x30 AgentStatus",
                         entry("REQ=%s", dump.c_str()));
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
        log<level::INFO>("ami-ipmi-oem: Cmd 0x31 GetDir",
                         entry("REQ=%s", dump.c_str()));
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
        log<level::INFO>("ami-ipmi-oem: DataBegin",
                         entry("REQ=%s", dump.c_str()));
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
        log<level::INFO>("ami-ipmi-oem: DataEnd",
                         entry("TOTAL=%zu REQ=%s", g_smbiosBuf.size(), dump.c_str()));
    } catch (...) {}
    if (g_state == MdrState::Idle)
    {
        log<level::WARNING>("ami-ipmi-oem: DataEnd outside session");
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
void setupGlobalOemFunctions() __attribute__((constructor));
void setupGlobalOemFunctions()
{
    log<level::INFO>("ami-ipmi-oem: registering AMI MDR handlers");

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
    }

    log<level::INFO>("ami-ipmi-oem: handlers registered");
}

