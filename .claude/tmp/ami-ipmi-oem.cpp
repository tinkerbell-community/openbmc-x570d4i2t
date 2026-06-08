// SPDX-License-Identifier: Apache-2.0
//
// ami-ipmi-oem.cpp — AMI MDR SMBIOS push handler for ASRock X570D4I-2T
//
// Uses the old-style C ipmi_register_callback() API to avoid crashes caused
// by the new-style template parameter-unpacking layer (SEGV on this ipmid).
//
// Confirmed AMI MDR V1 protocol from live POST captures (NetFn 0x32 / 0x3A):
//
//   WRITE FLOW (BIOS → BMC):
//     0x3D  Get MDR Status        — Handshake / capability advertisement
//     0x51  MDR Write Begin       — Reserve region, declare total payload size
//     0x52  MDR Write Chunk       — Stream data with 16-bit offset, repeated
//     0x53  MDR Write End         — Commit (BMC writes file + triggers sync)
//
//   READ FLOW (BMC → BIOS):
//     0x71  Get MDR Region Status — Per-region size, checksum, valid/lock flags
//     0x72  Get MDR Block         — Chunked SMBIOS read with offset
//
//   AGENT / DIRECTORY:
//     0x30  Agent Status          — Reports MDR/agent version + dataRequest flag
//     0x31  Get MDR Directory     — One-shot directory enumeration
//     0x5D  Legacy Begin/End      — Older AMI two-phase commit fallback
//
// DIRECTION OF DATA:
//   This AMI BIOS is a SMBIOS *reader*, not a writer. It opens a legacy 0x5D
//   handshake with zero payload, then reads via 0x72. The BMC is expected to
//   already have SMBIOS preloaded — either pushed by the host-side helper
//   (x570d4i2t-host-smbios-push) into /var/lib/smbios/smbios2, or committed
//   in a prior session. The write path (0x51/0x52/0x53) is kept for future
//   BIOS revisions that may push.
//
// REGION LAYOUT:
//   Region 0  SMBIOS structure-table (raw bytes from /var/lib/smbios/smbios2).
//             Only served when the in-session cache is valid; otherwise reads
//             return empty so BIOS knows "no data".
//   Region 1  SMBIOS 2.x Entry Point anchor — synthesized from the cached
//             table on each request: "_SM_" header, "_DMI_" intermediate,
//             total length, max struct size, structure count, valid checksums.
//             ALWAYS served, even when the cache is empty. An empty-cache
//             anchor reports size=0 / structures=0 with valid checksums so
//             BIOS sees a coherent "BMC has no SMBIOS" instead of a silent
//             response that it might interpret as a transport failure.
//
// CACHE VALIDITY:
//   On constructor, we probe /var/lib/smbios/smbios2; if the payload looks
//   like SMBIOS, set g_dataValidThisSession = true so reads work immediately
//   without waiting for a BIOS push. A successful 0x53 commit updates this
//   in place. A 0x5D phase=02 with zero buffered bytes is a BIOS handshake,
//   not a real write — it must not invalidate the cache.
//
// RESPONSE FRAMING (Cmd 0x72 GetBlock):
//   Raw data only — no length-byte prefix. IPMI's own response length carries
//   the chunk size. An earlier v4 prefixed `[len, data]`; AMI BIOS read the
//   length byte as the first SMBIOS byte, anchor validation failed, halted.
//
// NOTE: Do NOT use phosphor-logging/log.hpp here — the libphosphor_logging.so.1
// shipping on this BMC has an ABI mismatch that NULL-derefs in log<>().

#include <ipmid/api.h>
#include <systemd/sd-bus.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
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

// ── Command IDs ───────────────────────────────────────────────────────────────
static constexpr uint8_t cmdMdrAgentStatus  = 0x30;
static constexpr uint8_t cmdMdrGetDir       = 0x31;
static constexpr uint8_t cmdMdrGetStatus    = 0x3D;
static constexpr uint8_t cmdMdrWriteBegin   = 0x51;
static constexpr uint8_t cmdMdrWriteChunk   = 0x52;
static constexpr uint8_t cmdMdrWriteEnd     = 0x53;
static constexpr uint8_t cmdMdrSendDir      = 0x5D;
static constexpr uint8_t cmdMdrRegionStatus = 0x71;
static constexpr uint8_t cmdMdrGetBlock     = 0x72;

// REAL AMI BIOS push commands (reverse-engineered from
// SendInfoBmcIpmiDxe in AMI Aptio BIOS firmware X574I2T 2.59C).
// NetFn 0x3A only. Sequence per POST:
//   0xB2  SetBiosInfo    16-byte BIOS version block (version, date, revisions)
//   0xB5  SetSmbiosChunk variable-size SMBIOS structure-table fragments
//   0xF3  GetStatus      1-byte poll, repeated until BMC acks completion
static constexpr uint8_t cmdAmiSetBiosInfo  = 0xB2;
static constexpr uint8_t cmdAmiSetSmbios    = 0xB5;
static constexpr uint8_t cmdAmiGetStatus    = 0xF3;

// ── D-Bus / filesystem coordinates ───────────────────────────────────────────
static constexpr const char* kSmbiosDir   = "/var/lib/smbios";
static constexpr const char* kSmbiosFile  = "/var/lib/smbios/smbios2";
// MegaRAC-extracted baked SMBIOS dump (24-byte _SM3_ anchor + 1719-byte table)
static constexpr const char* kBakedDmp    = "/usr/share/x570d4i2t/smbios.dmp";
static constexpr size_t kSm3AnchorLen     = 24;
static constexpr const char* kMdrService = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kMdrPath    = "/xyz/openbmc_project/Smbios/MDR_V2";
static constexpr const char* kMdrIface   = "xyz.openbmc_project.Smbios.MDR_V2";
static constexpr const char* kSyncMethod = "AgentSynchronizeData";

// ── MDRSMBIOSHeader (smbios-mdrv2 on-disk file format) ───────────────────────
struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;   // 0x02
    uint8_t  mdrType;      // 0x02 = SMBIOS
    uint32_t timestamp;    // epoch seconds (LE)
    uint32_t dataSize;     // payload bytes (LE)
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10, "MDRSMBIOSHeader size mismatch");

// ── Region IDs ────────────────────────────────────────────────────────────────
static constexpr uint8_t kRegionSmbios = 0;
static constexpr uint8_t kRegionMeta   = 1;

// Max raw bytes per GetBlock response chunk. BIOS validates Region 1 (the
// 31-byte anchor) in one shot, then expects Region 0 in one shot too — if
// the first chunk isn't the complete table, BIOS halts after reading it
// instead of paginating. Match what 0x3D advertises (0x1000 = 4096) so the
// full 1719-byte X570D4I-2T table fits in one response. ipmid's response
// buffer is multi-KB; AST2500 KCS handles this fine.
static constexpr size_t  kMaxReadChunk = 4096;

// ── Assembly buffer state (write path) ────────────────────────────────────────
enum class MdrState : uint8_t { Idle, Open, Receiving };
static MdrState             g_state    = MdrState::Idle;
static uint32_t             g_expected = 0;
static std::vector<uint8_t> g_smbiosBuf;

// ── In-session validity (read path) ───────────────────────────────────────────
// g_dataValidThisSession — true whenever cache (real OR skeleton) is loaded.
//                           Gates whether GetBlock serves the cache or empty.
// g_hasRealSmbios         — true only when cache holds a real SMBIOS table.
//                           When false, status replies advertise "need data"
//                           so BIOS proceeds with its push instead of trusting
//                           our placeholder skeleton as authoritative.
static bool     g_dataValidThisSession = false;
static bool     g_hasRealSmbios        = false;
static uint16_t g_dataChecksum         = 0;
static uint16_t g_dataSize             = 0;

// ── Chunked-transfer state for 0xA0 SetMdrPos ─────────────────────────────────
// AMI BIOS uses 0xA0 as a structured fragment-transfer command because KCS
// caps individual packets at ~64–240 bytes. Each call carries:
//   [seq:1, flags:1, chunkLen:2 LE, totalLen:2 LE, data:N]
// Flag bits:
//   0x01 = Start  (resets cursor + sets totalLen)
//   0x02 = Continue
//   0x04 = End    (commits)
// Response: [CC:1, nextExpectedSeq:1]
//   CC=0x00 OK / 0xC7 out-of-order / 0xC8 accumulation length mismatch
static uint8_t              g_chunkExpectedSeq = 0;
static uint16_t             g_chunkTotalLen    = 0;
static uint16_t             g_chunkAccumLen    = 0;
static std::vector<uint8_t> g_chunkBuf;

static constexpr uint8_t kFlagStart            = 0x01;
static constexpr uint8_t kFlagContinue         = 0x02;
static constexpr uint8_t kFlagEnd              = 0x04;

static constexpr uint8_t kChunkCcOK            = 0x00;
static constexpr uint8_t kChunkCcOutOfOrder    = 0xC7;
static constexpr uint8_t kChunkCcLengthMismatch = 0xC8;

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

// 16-bit additive checksum, matches what most AMI/Intel MDR variants compute.
static uint16_t mdrChecksum(const uint8_t* data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += data[i];
    return static_cast<uint16_t>(sum & 0xFFFF);
}

// Sanity-check that the assembled buffer looks like an SMBIOS structure table:
// first record's Type must be 0..127 (SMBIOS reserves 128+ for OEM) and the
// formatted-area length must be in the 4..255 range.
static bool looksLikeSmbios(const std::vector<uint8_t>& buf)
{
    if (buf.size() < 8) return false;
    uint8_t firstType = buf[0];
    uint8_t firstLen  = buf[1];
    if (firstType > 127) return false;
    if (firstLen < 4 || firstLen > 255) return false;
    return true;
}

// Walk the SMBIOS structure table to count records and find the largest one.
// Each record is `formatted_area` bytes followed by a string set terminated
// by a double-NUL. End-of-table marker is type 127.
struct SmbiosScan
{
    uint16_t structCount   = 0;
    uint16_t maxStructSize = 0;
};
static SmbiosScan scanSmbios(const std::vector<uint8_t>& buf)
{
    SmbiosScan s;
    size_t pos = 0;
    while (pos + 4 <= buf.size())
    {
        uint8_t type    = buf[pos];
        uint8_t fmtLen  = buf[pos + 1];
        if (fmtLen < 4) break;
        size_t end = pos + fmtLen;
        // Skip the string area: read until a double-NUL (or buffer end).
        while (end + 1 < buf.size() && !(buf[end] == 0 && buf[end + 1] == 0))
            end++;
        end += 2;
        if (end > buf.size()) end = buf.size();
        uint16_t totalLen = static_cast<uint16_t>(end - pos);
        if (totalLen > s.maxStructSize) s.maxStructSize = totalLen;
        s.structCount++;
        if (type == 127) break;
        pos = end;
    }
    return s;
}

// Synthesize the SMBIOS 2.x Entry Point anchor for the cached structure table.
// Returns exactly 31 bytes. Both checksums (main + intermediate) are computed
// so the sum of the relevant byte ranges equals zero — BIOS verifies these.
// Accepts cached data with OR without a leading _SM_/_SM3_ anchor; if present,
// the anchor is skipped before scanning the structure table.
static std::vector<uint8_t> buildEntryPoint(const std::vector<uint8_t>& cached)
{
    std::vector<uint8_t> tableOnly;
    if (cached.size() >= 5 && cached[0] == '_' && cached[1] == 'S' &&
        cached[2] == 'M' && cached[3] == '3' && cached[4] == '_')
        tableOnly.assign(cached.begin() + 24, cached.end());      // SMBIOS 3.x: 24-byte anchor
    else if (cached.size() >= 5 && cached[0] == '_' && cached[1] == 'S' &&
             cached[2] == 'M' && cached[3] == '_')
        tableOnly.assign(cached.begin() + 31, cached.end());      // SMBIOS 2.x: 31-byte anchor
    else
        tableOnly = cached;

    SmbiosScan scan = scanSmbios(tableOnly);
    std::vector<uint8_t> ep(31, 0);

    // _SM_ anchor
    ep[0] = '_'; ep[1] = 'S'; ep[2] = 'M'; ep[3] = '_';
    // ep[4] = main checksum, filled in below
    ep[5]  = 0x1F;                                  // entry point length
    ep[6]  = 3;                                      // SMBIOS major (3.0)
    ep[7]  = 0;                                      // SMBIOS minor
    ep[8]  = scan.maxStructSize       & 0xFF;        // max struct size LSB
    ep[9]  = (scan.maxStructSize >> 8) & 0xFF;       // max struct size MSB
    ep[10] = 0;                                      // entry point revision
    // ep[11..15] formatted area — reserved zeros

    // _DMI_ intermediate anchor
    ep[16] = '_'; ep[17] = 'D'; ep[18] = 'M'; ep[19] = 'I'; ep[20] = '_';
    // ep[21] = intermediate checksum, filled in below
    uint16_t tblLen = static_cast<uint16_t>(tableOnly.size());
    ep[22] = tblLen       & 0xFF;                    // structure table length LSB
    ep[23] = (tblLen >> 8) & 0xFF;                   // structure table length MSB
    // ep[24..27] structure table address — placeholder (BIOS rewrites)
    ep[28] = scan.structCount       & 0xFF;          // number of structures LSB
    ep[29] = (scan.structCount >> 8) & 0xFF;         // number of structures MSB
    ep[30] = 0x30;                                   // BCD revision 3.0

    // Intermediate checksum: sum of bytes [0x10..0x1E] must equal 0.
    uint8_t sum = 0;
    for (int i = 0x10; i <= 0x1E; ++i) sum += ep[i];
    ep[0x15] = static_cast<uint8_t>(-static_cast<int>(sum));
    // Main checksum: sum of all 31 bytes [0x00..0x1E] must equal 0.
    sum = 0;
    for (int i = 0; i <= 0x1E; ++i) sum += ep[i];
    ep[0x04] = static_cast<uint8_t>(-static_cast<int>(sum));

    return ep;
}

// Strip the 10-byte MDRSMBIOSHeader and return the raw SMBIOS bytes.
static std::vector<uint8_t> loadCachedSmbios()
{
    std::vector<uint8_t> out;
    std::ifstream ifs(kSmbiosFile, std::ios::binary);
    if (!ifs) return out;
    MDRSMBIOSHeader header;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (ifs.gcount() != sizeof(header)) return out;
    out.resize(header.dataSize);
    ifs.read(reinterpret_cast<char*>(out.data()), header.dataSize);
    out.resize(static_cast<size_t>(ifs.gcount()));
    return out;
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

// Probe the cached file at startup so reads work even when BIOS never pushes.
// Source could be a prior 0x53 commit or the host-side helper push.
static void primeFromCache()
{
    std::vector<uint8_t> data = loadCachedSmbios();
    // Any cached file larger than the 30-byte skeleton is treated as real
    // SMBIOS — could be from a previous baked-dmp seed or a future BIOS push.
    if (data.size() > 30 && looksLikeSmbios(data))
    {
        g_dataChecksum         = mdrChecksum(data.data(), data.size());
        g_dataSize             = static_cast<uint16_t>(data.size());
        g_dataValidThisSession = true;
        g_hasRealSmbios        = true;
        LOG_INFO("primeFromCache: REAL SMBIOS loaded (%u bytes, chk=0x%04X)",
                 g_dataSize, g_dataChecksum);
        return;
    }

    // Seed from baked MegaRAC dmp. Keep the full file (with _SM3_ anchor) —
    // smbios-mdrv2 scans for the anchor string and rejects files without one.
    {
        std::ifstream baked(kBakedDmp, std::ios::binary);
        if (baked)
        {
            std::vector<uint8_t> dmp((std::istreambuf_iterator<char>(baked)),
                                      std::istreambuf_iterator<char>());
            bool hasAnchor = (dmp.size() >= 5 &&
                              dmp[0] == '_' && dmp[1] == 'S' && dmp[2] == 'M' &&
                              (dmp[3] == '3' || dmp[3] == '_'));
            if (dmp.size() > kSm3AnchorLen && hasAnchor)
            {
                g_smbiosBuf = dmp;
                if (writeSmbiosFile())
                {
                    g_dataChecksum         = mdrChecksum(g_smbiosBuf.data(), g_smbiosBuf.size());
                    g_dataSize             = static_cast<uint16_t>(g_smbiosBuf.size());
                    g_dataValidThisSession = true;
                    g_hasRealSmbios        = true;
                    LOG_INFO("primeFromCache: seeded from baked dmp — %u bytes total (anchor preserved, chk=0x%04X)",
                             g_dataSize, g_dataChecksum);
                    triggerMdrSync();
                    g_smbiosBuf.clear();
                    return;
                }
                LOG_WARN("primeFromCache: baked dmp present but write failed");
                g_smbiosBuf.clear();
            }
            else
            {
                LOG_WARN("primeFromCache: baked dmp at %s is %zu bytes but no _SM_/_SM3_ anchor",
                         kBakedDmp, dmp.size());
            }
        }
        else
        {
            LOG_INFO("primeFromCache: no baked dmp at %s — falling back to skeleton",
                     kBakedDmp);
        }
    }

    // Last-resort: 30-byte Type 0 + Type 127 placeholder so BIOS doesn't halt.
    if (data.size() >= 8 && looksLikeSmbios(data))
        LOG_INFO("primeFromCache: existing %zu-byte skeleton — refreshing in place",
                 data.size());
    else if (!data.empty())
        LOG_WARN("primeFromCache: cached %zu bytes don't look like SMBIOS — replacing with skeleton",
                 data.size());
    else
        LOG_INFO("primeFromCache: priming minimal fallback skeleton");

    // Type 0 (BIOS Information, header-only, no strings) + Type 127 (End of Table).
    // This is malformed enough to be useless for inventory but coherent enough
    // that BIOS sees a valid table layout instead of giving up. The host-side
    // helper will overwrite this with a real table once Linux is up.
    // Type 0 formatted area must be exactly Length(0x16=22) bytes total
    // including the 4-byte header. That's 18 body bytes after the header.
    static const std::vector<uint8_t> fallback = {
        0x00, 0x16, 0x00, 0x00,             // Type 0, Length 22, Handle 0x0000
        0x01, 0x02, 0x00, 0x00, 0x00, 0x00, // body bytes 1..6  (string refs + reserved)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // body bytes 7..12
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // body bytes 13..18  (total: 18 body bytes)
        0x00, 0x00,                         // Type 0 string area double-NUL terminator
        0x7F, 0x04, 0x01, 0x00,             // Type 127 (End of Table), Length 4, Handle 0x0001
        0x00, 0x00,                         // Type 127 string area double-NUL terminator
    };
    static_assert(sizeof(decltype(fallback)::value_type) == 1, "byte vector");
    g_smbiosBuf            = fallback;
    g_dataChecksum         = mdrChecksum(g_smbiosBuf.data(), g_smbiosBuf.size());
    g_dataSize             = static_cast<uint16_t>(g_smbiosBuf.size());
    g_dataValidThisSession = true;
    // Persist the skeleton so loadCachedSmbios() on subsequent reads matches.
    writeSmbiosFile();
    g_smbiosBuf.clear();
}

static void doCommit()
{
    // A 0x5D phase=02 handshake (BIOS probe) arrives with empty buffer — that is
    // not a real commit; just reset state and leave any cached data alone.
    if (g_smbiosBuf.empty())
    {
        LOG_INFO("commit: empty buffer — treating as handshake, preserving cache");
        g_state    = MdrState::Idle;
        g_expected = 0;
        return;
    }

    if (g_smbiosBuf.size() < 8)
    {
        LOG_WARN("commit: buffer too small (%zu bytes) — discarding", g_smbiosBuf.size());
    }
    else if (!looksLikeSmbios(g_smbiosBuf))
    {
        LOG_WARN("commit: buffer (%zu bytes, first=%s) doesn't look like SMBIOS — discarding",
                 g_smbiosBuf.size(),
                 hexDump(g_smbiosBuf.data(), 8).c_str());
    }
    else if (writeSmbiosFile())
    {
        g_dataChecksum         = mdrChecksum(g_smbiosBuf.data(), g_smbiosBuf.size());
        g_dataSize             = static_cast<uint16_t>(g_smbiosBuf.size());
        g_dataValidThisSession = true;
        g_hasRealSmbios        = true;  // committed via BIOS push — real data
        triggerMdrSync();
        LOG_INFO("commit: REAL SMBIOS valid this session (%u bytes, chk=0x%04X)",
                 g_dataSize, g_dataChecksum);
    }

    g_smbiosBuf.clear();
    g_state    = MdrState::Idle;
    g_expected = 0;
}

// ── Handler: Cmd 0x3D — Get MDR Status (handshake) ───────────────────────────
static ipmi_ret_t handlerMdrGetStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x3D GetMdrStatus REQ[%zu]=%s state=%d buf=%zu valid=%d real=%d",
             *data_len, hexDump(req, *data_len).c_str(),
             (int)g_state, g_smbiosBuf.size(),
             g_dataValidThisSession ? 1 : 0, g_hasRealSmbios ? 1 : 0);

    uint8_t* rsp = static_cast<uint8_t*>(response);
    // Status byte — exact AMI semantics are undocumented; iterate via env.
    //   AMI_MDR_NEED_DATA_STATUS=N  → use N (hex/dec) when !g_hasRealSmbios.
    // Set in /etc/systemd/system/phosphor-ipmi-host.service.d/override.conf.
    static uint8_t needDataStatus = []() {
        const char* env = std::getenv("AMI_MDR_NEED_DATA_STATUS");
        if (!env) return uint8_t{0x01};
        unsigned long v = std::strtoul(env, nullptr, 0);
        return static_cast<uint8_t>(v & 0xFF);
    }();
    rsp[0] = g_hasRealSmbios ? 0x00 : needDataStatus;
    rsp[1] = 0x00;  // Max chunk LSB ) 0x1000 = 4 KiB max per Write Chunk
    rsp[2] = 0x10;  // Max chunk MSB )
    rsp[3] = 0x01;  // MDR Version = 1
    *data_len = 4;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x51 — MDR Write Begin ──────────────────────────────────────
static ipmi_ret_t handlerMdrWriteBegin(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t,
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
    g_smbiosBuf.reserve(declared);
    g_expected = declared;
    g_state    = MdrState::Open;
    LOG_INFO("MdrWriteBegin: session open, expecting %u bytes", declared);

    // CC only. Extra payload bytes crash the AMI BIOS state machine.
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x52 — MDR Write Chunk ──────────────────────────────────────
static ipmi_ret_t handlerMdrWriteChunk(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t,
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

    const uint8_t* payload = req + 3;            // skip [regionId, offLo, offHi]
    size_t   payloadLen   = len - 3;
    uint16_t offset       = static_cast<uint16_t>(req[1]) |
                            (static_cast<uint16_t>(req[2]) << 8);
    size_t   requiredSize = offset + payloadLen;

    if (requiredSize > 256 * 1024)
    {
        LOG_ERR("Cmd 0x52 MdrWriteChunk: offset/len too large (%zu bytes)", requiredSize);
        *data_len = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    if (requiredSize > g_smbiosBuf.size())
        g_smbiosBuf.resize(requiredSize, 0);  // zero-fill gaps from out-of-order chunks

    std::copy(payload, payload + payloadLen, g_smbiosBuf.begin() + offset);
    g_state = MdrState::Receiving;
    LOG_INFO("Cmd 0x52 MdrWriteChunk: offset=0x%04X wrote=%zu total_cap=%zu",
             offset, payloadLen, g_smbiosBuf.size());

    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x53 — MDR Write End (commit) ───────────────────────────────
static ipmi_ret_t handlerMdrWriteEnd(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x53 MdrWriteEnd REQ[%zu]=%s total_buf=%zu",
             *data_len, hexDump(req, *data_len).c_str(), g_smbiosBuf.size());
    doCommit();
    *data_len = 0;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x71 — Get MDR Region Status ────────────────────────────────
// Response (10 bytes):
//   [MdrVer, RegionId, ValidFlag, LockFlag, UpdateCnt,
//    SizeLSB, SizeMSB, UsedLSB, UsedMSB, ChecksumLSB]
//
// BIOS calls this BEFORE GetBlock to learn size + checksum and decide whether
// to read or rewrite. Reporting Valid=0 + Size=0 here is what makes the BIOS
// commit to its push path when our session has no data.
static ipmi_ret_t handlerMdrRegionStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    uint8_t regionId = (*data_len > 0) ? req[0] : 0;
    LOG_INFO("Cmd 0x71 RegionStatus Region=%u REQ[%zu]=%s",
             regionId, *data_len, hexDump(req, *data_len).c_str());

    // Region 1 (anchor) is ALWAYS valid — synthesized on demand.
    // Region 0 (SMBIOS table) only reports valid when we have REAL data;
    // skeleton data must NOT be advertised as authoritative or BIOS skips push.
    bool valid = (regionId == kRegionMeta) ? g_dataValidThisSession
               : (regionId == kRegionSmbios) ? g_hasRealSmbios
               : false;
    uint16_t size = valid ? g_dataSize : 0;
    uint16_t chk  = valid ? g_dataChecksum : 0;

    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;                                          // MDR Version
    rsp[1] = regionId;                                       // RegionId echo
    rsp[2] = valid ? 0x01 : 0x00;                            // Valid
    rsp[3] = (g_state != MdrState::Idle) ? 0x01 : 0x00;      // Lock
    rsp[4] = 0x00;                                           // UpdateCount
    rsp[5] = size & 0xFF;                                    // Size LSB
    rsp[6] = (size >> 8) & 0xFF;                             // Size MSB
    rsp[7] = size & 0xFF;                                    // Used LSB
    rsp[8] = (size >> 8) & 0xFF;                             // Used MSB
    rsp[9] = chk & 0xFF;                                     // Checksum LSB
    *data_len = 10;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x72 — Get MDR Block (read cached SMBIOS) ───────────────────
// Response: raw data only (no length prefix).
//   Region 0 — chunk of SMBIOS structure-table cache at the requested offset
//   Region 1 — synthesized 31-byte SMBIOS 2.x entry-point anchor (offset 0
//              returns all 31 bytes; further offsets read into the anchor)
// Empty response (*data_len = 0) signals end-of-data.
static ipmi_ret_t handlerMdrGetBlock(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    if (*data_len < 3)
    {
        *data_len = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    const uint8_t* req = static_cast<const uint8_t*>(request);
    uint8_t  regionId = req[0];
    uint16_t offset   = static_cast<uint16_t>(req[1]) |
                        (static_cast<uint16_t>(req[2]) << 8);
    uint8_t* rsp = static_cast<uint8_t*>(response);

    if (regionId != kRegionSmbios && regionId != kRegionMeta)
    {
        LOG_INFO("Cmd 0x72 GetBlock Region=%u Off=%u — unknown region, no data",
                 regionId, offset);
        *data_len = 0;
        return IPMI_CC_OK;
    }

    // Pick the source bytes based on region.
    //   Region 1: always synthesize anchor — empty cache yields a size=0 anchor.
    //             Anchor describes either the real or skeleton table (whichever
    //             is cached) so checksums always validate.
    //   Region 0: only serve when we have REAL SMBIOS. If only the skeleton
    //             is present, return empty — otherwise BIOS reads the skeleton,
    //             validates it as a real Type 0+127 table, and skips the push.
    std::vector<uint8_t> source;
    if (regionId == kRegionMeta)
    {
        // Anchor describes whatever's in the cache (real OR skeleton). BIOS
        // uses Region 1 to validate Region 0 sizes/checksums match — passing
        // a zero-table anchor while Region 0 has bytes makes BIOS fail check.
        source = buildEntryPoint(loadCachedSmbios());   // always 31 bytes
    }
    else
    {
        // Region 0 serves the SMBIOS STRUCTURE TABLE only — strip the entry-
        // point anchor (`_SM_` or `_SM3_`) at the start of the cached file.
        // Region 1 already carries the anchor; including it in Region 0 too
        // gives BIOS two conflicting anchors and it halts after the first read.
        if (!g_dataValidThisSession)
        {
            LOG_INFO("Cmd 0x72 GetBlock Region=0 Off=%u — no cache at all",
                     offset);
            *data_len = 0;
            return IPMI_CC_OK;
        }
        auto cached = loadCachedSmbios();
        size_t anchorLen = 0;
        if (cached.size() >= 5 && cached[0] == '_' && cached[1] == 'S' &&
            cached[2] == 'M')
        {
            if (cached[3] == '3' && cached[4] == '_')
                anchorLen = 24;                        // SMBIOS 3.x
            else if (cached[3] == '_')
                anchorLen = 31;                        // SMBIOS 2.x
        }
        source.assign(cached.begin() + anchorLen, cached.end());
    }

    if (offset >= source.size())
    {
        LOG_INFO("Cmd 0x72 GetBlock Region=%u Off=%u — past end of %zu-byte region",
                 regionId, offset, source.size());
        *data_len = 0;
        return IPMI_CC_OK;
    }

    size_t sendLen = std::min(source.size() - offset, kMaxReadChunk);
    std::memcpy(rsp, source.data() + offset, sendLen);
    *data_len = sendLen;
    LOG_INFO("Cmd 0x72 GetBlock Region=%u Off=%u sent=%zu/%zu",
             regionId, offset, sendLen, source.size());
    return IPMI_CC_OK;
}

// Replace the strings of the first Type 0 (BIOS Information) record in
// /var/lib/smbios/smbios2 with the provided vendor/version/date.
// Updates MDR header dataSize. Returns true on success.
static bool replaceType0Strings(const std::string& vendor,
                                const std::string& version,
                                const std::string& date)
{
    std::ifstream ifs(kSmbiosFile, std::ios::binary);
    if (!ifs) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    ifs.close();
    constexpr size_t hdrLen = 10;
    if (file.size() < hdrLen + 32) return false;

    // Skip MDR header + SMBIOS anchor.
    size_t pos = hdrLen;
    if (file[pos] == '_' && file[pos+1] == 'S' && file[pos+2] == 'M')
    {
        if (file[pos+3] == '3' && file[pos+4] == '_') pos += 24;
        else if (file[pos+3] == '_')                  pos += 31;
    }

    // First record must be Type 0; if not, bail.
    if (pos + 4 > file.size() || file[pos] != 0) return false;
    uint8_t fmtLen = file[pos + 1];
    if (fmtLen < 4 || pos + fmtLen > file.size()) return false;

    // String area begins right after the formatted area, ends at \0\0.
    size_t saStart = pos + fmtLen;
    size_t saEnd   = saStart;
    while (saEnd + 1 < file.size() &&
           !(file[saEnd] == 0 && file[saEnd + 1] == 0))
        saEnd++;
    saEnd += 2;   // include the terminator
    if (saEnd > file.size()) return false;

    // Build the new string area: vendor\0version\0date\0\0
    std::vector<uint8_t> newSA;
    auto pushString = [&](const std::string& s)
    {
        if (s.empty()) newSA.push_back(0);  // empty string slot
        else
        {
            newSA.insert(newSA.end(), s.begin(), s.end());
            newSA.push_back(0);
        }
    };
    pushString(vendor);
    pushString(version);
    pushString(date);
    newSA.push_back(0);  // double-NUL terminator

    LOG_INFO("smbios2: Type 0 vendor=\"%s\" version=\"%s\" date=\"%s\" — string area %zu → %zu bytes",
             vendor.c_str(), version.c_str(), date.c_str(),
             saEnd - saStart, newSA.size());

    file.erase(file.begin() + saStart, file.begin() + saEnd);
    file.insert(file.begin() + saStart, newSA.begin(), newSA.end());

    uint32_t newDataSize = static_cast<uint32_t>(file.size() - hdrLen);
    file[6] = newDataSize & 0xFF;
    file[7] = (newDataSize >> 8) & 0xFF;
    file[8] = (newDataSize >> 16) & 0xFF;
    file[9] = (newDataSize >> 24) & 0xFF;

    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    LOG_INFO("smbios2: %zu bytes (payload %u)", file.size(), newDataSize);
    return true;
}

// ── Handler: Cmd 0xB2 — AMI SetBiosInfo (16-byte BIOS version block) ─────────
// Decodes [biosType, majorBCD, minorBCD, revChar, dateBCD×4, dwords...] into
// a "M.mmR" version string + "MM/DD/YYYY" date, then overlays Type 0's
// version and release-date strings in /var/lib/smbios/smbios2.
static ipmi_ret_t handlerAmiSetBiosInfo(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0xB2 SetBiosInfo REQ[%zu]=%s",
             *data_len, hexDump(req, *data_len).c_str());

    if (*data_len >= 8)
    {
        // Build version string "M.mmR" — major BCD digit, minor BCD pair, revision char.
        char ver[16];
        uint8_t major   = req[1];                // BCD single digit (0..9)
        uint8_t minorHi = (req[2] >> 4) & 0xF;   // BCD high nibble
        uint8_t minorLo =  req[2]       & 0xF;   // BCD low nibble
        uint8_t revCh   = req[3];                // ASCII char (e.g. 'C' = 0x43)
        if (revCh >= 0x20 && revCh < 0x7F && revCh != ' ')
            snprintf(ver, sizeof(ver), "%u.%u%u%c",
                     major, minorHi, minorLo, revCh);
        else
            snprintf(ver, sizeof(ver), "%u.%u%u",
                     major, minorHi, minorLo);

        // Date: 4 BCD bytes. Layout per AMI SendInfoBmcIpmiDxe = MM DD YY YY
        // (live trace `0A 20 12 0E` = `10 20 12 14` → 10/20/12/14 → "10/20/1214"
        // but BIOS X574I2T 2.59C release date is real-world 2024-10-12, so the
        // BCD layout here is observed as [MM, ??, ??, YY]; without an authoritative
        // spec, we surface them raw and let the user adjust if needed).
        auto bcd2 = [](uint8_t b) -> std::string {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02X", b);
            return std::string(buf);
        };
        std::string date = bcd2(req[4]) + "/" + bcd2(req[5]) + "/" +
                           bcd2(req[6]) + bcd2(req[7]);

        LOG_INFO("0xB2: version=\"%s\" date=\"%s\"", ver, date.c_str());
        if (replaceType0Strings("American Megatrends Inc.", ver, date))
            triggerMdrSync();
    }

    static_cast<uint8_t*>(response)[0] = 0x00;
    *data_len = 1;
    return IPMI_CC_OK;
}

// Walk SMBIOS structures in /var/lib/smbios/smbios2 and replace the next
// placeholder string ("To Be Filled By O.E.M." or "Default string") inside
// any structure's string-set with `newStr`. Returns true on success.
//
// Unlike a linear byte scan, this respects SMBIOS framing: we step record
// by record, only look at string-area bytes (never the binary formatted
// area), and report which Type+handle was touched. That makes the overlay
// deterministic in the SMBIOS sense — string N of structure N goes where
// SMBIOS says it goes, not wherever the bytes happen to match.
//
// MDR header dataSize is updated to reflect the new file length.
static bool replaceNextPlaceholder(const std::string& newStr)
{
    static const std::vector<std::string> placeholders = {
        "To Be Filled By O.E.M.",
        "Default string",
    };
    // Trivial no-op guard.
    for (const auto& ph : placeholders)
        if (newStr == ph) return false;

    std::ifstream ifs(kSmbiosFile, std::ios::binary);
    if (!ifs) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    ifs.close();
    constexpr size_t hdrLen = 10;
    if (file.size() < hdrLen + 32) return false;

    // Skip MDR header, then skip SMBIOS entry-point anchor if present.
    size_t tableStart = hdrLen;
    if (file[hdrLen + 0] == '_' && file[hdrLen + 1] == 'S' &&
        file[hdrLen + 2] == 'M')
    {
        if (file[hdrLen + 3] == '3' && file[hdrLen + 4] == '_')
            tableStart = hdrLen + 24;                      // SMBIOS 3.x
        else if (file[hdrLen + 3] == '_')
            tableStart = hdrLen + 31;                      // SMBIOS 2.x
    }

    size_t pos = tableStart;
    while (pos + 4 <= file.size())
    {
        uint8_t  type   = file[pos];
        uint8_t  fmtLen = file[pos + 1];
        uint16_t handle = static_cast<uint16_t>(file[pos + 2]) |
                          (static_cast<uint16_t>(file[pos + 3]) << 8);
        if (fmtLen < 4 || pos + fmtLen > file.size()) break;

        size_t strStart = pos + fmtLen;                    // start of string set
        size_t cursor   = strStart;

        // Walk one string at a time until we hit the double-NUL terminator.
        while (cursor < file.size())
        {
            if (file[cursor] == 0)
            {
                // End of a string. If followed by another NUL → end of set.
                if (cursor + 1 < file.size() && file[cursor + 1] == 0)
                {
                    pos = cursor + 2;                      // next record
                    break;
                }
                cursor++;
                continue;
            }
            // Measure this string and test against the placeholder list.
            size_t end = cursor;
            while (end < file.size() && file[end] != 0) end++;
            size_t sLen = end - cursor;

            for (const auto& ph : placeholders)
            {
                if (sLen != ph.size()) continue;
                if (std::memcmp(file.data() + cursor, ph.data(), sLen) != 0) continue;

                LOG_INFO("smbios2: Type %u handle=0x%04X — replacing \"%s\" with \"%s\" @offset %zu",
                         type, handle, ph.c_str(), newStr.c_str(),
                         cursor - hdrLen);
                file.erase(file.begin() + cursor,
                           file.begin() + cursor + sLen);
                file.insert(file.begin() + cursor,
                            newStr.begin(), newStr.end());

                uint32_t newDataSize = static_cast<uint32_t>(file.size() - hdrLen);
                file[6] = newDataSize & 0xFF;
                file[7] = (newDataSize >> 8) & 0xFF;
                file[8] = (newDataSize >> 16) & 0xFF;
                file[9] = (newDataSize >> 24) & 0xFF;

                std::ofstream ofs(kSmbiosFile,
                                  std::ios::binary | std::ios::trunc);
                if (!ofs) return false;
                ofs.write(reinterpret_cast<const char*>(file.data()),
                          static_cast<std::streamsize>(file.size()));
                LOG_INFO("smbios2: %zu bytes (payload %u)",
                         file.size(), newDataSize);
                return true;
            }
            cursor = end;                                  // skip past this string
        }
        // Defensive: if string-set never terminated, abandon walk.
        if (pos <= strStart) break;
        if (type == 127) break;                            // end-of-table reached
    }

    LOG_INFO("smbios2: no placeholder left for \"%s\"", newStr.c_str());
    return false;
}

// ── Handler: Cmd 0xB5 — AMI SetSmbiosChunk (overlay onto baked dmp) ──────────
// BIOS pushes one SMBIOS string fragment per call, framed as `\0STRING\0`.
// We extract the first non-empty string and substitute it for the next
// placeholder in /var/lib/smbios/smbios2. The baked dmp from MegaRAC is
// pre-seeded with "To Be Filled By O.E.M." / "Default string" entries that
// get progressively replaced as BIOS pushes its real values.
static ipmi_ret_t handlerAmiSetSmbios(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    size_t len = *data_len;
    LOG_INFO("Cmd 0xB5 SetSmbiosChunk len=%zu first=%s",
             len, hexDump(req, std::min(len, size_t{16})).c_str());

    // Extract first non-empty NUL-delimited string from the chunk.
    std::string s;
    bool startedString = false;
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t b = req[i];
        if (b == 0)
        {
            if (startedString) break;       // end of string
            continue;                       // leading NUL — skip
        }
        startedString = true;
        if (b >= 0x20 && b < 0x7F)          // printable ASCII only
            s.push_back(static_cast<char>(b));
    }

    if (!s.empty() && replaceNextPlaceholder(s))
    {
        triggerMdrSync();
    }

    static_cast<uint8_t*>(response)[0] = 0x00;
    *data_len = 1;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0xA0 — AMI SetMdrPos (chunked SMBIOS transfer) ──────────────
// Request: [seq:1, flags:1, chunkLen:2 LE, totalLen:2 LE, data:N]
// Response: [CC:1, nextExpectedSeq:1]
// On End flag, accumulated buffer is written to /var/lib/smbios/smbios2.
static ipmi_ret_t handlerAmiSetMdrPos(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    uint8_t* rsp = static_cast<uint8_t*>(response);

    LOG_INFO("Cmd 0xA0 SetMdrPos REQ[%zu]=%s",
             *data_len, hexDump(req, *data_len).c_str());

    if (*data_len < 6)
    {
        LOG_WARN("0xA0: short header (%zu < 6)", *data_len);
        rsp[0] = kChunkCcOutOfOrder;
        rsp[1] = g_chunkExpectedSeq;
        *data_len = 2;
        return IPMI_CC_OK;
    }

    uint8_t  seq      = req[0];
    uint8_t  flags    = req[1];
    uint16_t chunkLen = static_cast<uint16_t>(req[2]) |
                        (static_cast<uint16_t>(req[3]) << 8);
    uint16_t totalLen = static_cast<uint16_t>(req[4]) |
                        (static_cast<uint16_t>(req[5]) << 8);

    LOG_INFO("0xA0: seq=%u flags=0x%02X chunkLen=%u totalLen=%u accum=%u",
             seq, flags, chunkLen, totalLen, g_chunkAccumLen);

    // Start flag: reset cursor + declare total length BEFORE seq check.
    if (flags & kFlagStart)
    {
        g_chunkBuf.clear();
        if (totalLen > 0) g_chunkBuf.reserve(totalLen);
        g_chunkTotalLen    = totalLen;
        g_chunkAccumLen    = 0;
        g_chunkExpectedSeq = 0;
        LOG_INFO("0xA0 START: cursor reset, expecting %u total bytes", totalLen);
    }

    // Sequence check.
    if (seq != g_chunkExpectedSeq)
    {
        LOG_WARN("0xA0: out-of-order seq (got %u, expected %u)",
                 seq, g_chunkExpectedSeq);
        rsp[0] = kChunkCcOutOfOrder;
        rsp[1] = g_chunkExpectedSeq;
        *data_len = 2;
        return IPMI_CC_OK;
    }

    // Inline data after the 6-byte header.
    size_t avail = (*data_len > 6) ? *data_len - 6 : 0;
    size_t toAppend = std::min<size_t>(chunkLen, avail);
    if (toAppend > 0)
    {
        g_chunkBuf.insert(g_chunkBuf.end(),
                          req + 6, req + 6 + toAppend);
        g_chunkAccumLen = static_cast<uint16_t>(g_chunkAccumLen + toAppend);
    }

    // End flag: validate total length and commit.
    if (flags & kFlagEnd)
    {
        if (g_chunkAccumLen != g_chunkTotalLen)
        {
            LOG_WARN("0xA0 END: length mismatch (got %u, expected %u)",
                     g_chunkAccumLen, g_chunkTotalLen);
            rsp[0] = kChunkCcLengthMismatch;
            rsp[1] = 0;
            *data_len = 2;
            return IPMI_CC_OK;
        }
        LOG_INFO("0xA0 END: committing %u bytes to %s",
                 g_chunkAccumLen, kSmbiosFile);
        if (g_chunkBuf.size() >= 8 && looksLikeSmbios(g_chunkBuf))
        {
            g_smbiosBuf = g_chunkBuf;
            if (writeSmbiosFile())
            {
                g_dataChecksum         = mdrChecksum(g_smbiosBuf.data(),
                                                     g_smbiosBuf.size());
                g_dataSize             = static_cast<uint16_t>(g_smbiosBuf.size());
                g_dataValidThisSession = true;
                g_hasRealSmbios        = true;
                triggerMdrSync();
                LOG_INFO("0xA0: committed %u-byte SMBIOS (chk=0x%04X)",
                         g_dataSize, g_dataChecksum);
            }
            g_smbiosBuf.clear();
        }
        else
        {
            LOG_WARN("0xA0: accumulated buffer (%zu bytes) doesn't look like SMBIOS",
                     g_chunkBuf.size());
        }
        g_chunkBuf.clear();
        g_chunkAccumLen    = 0;
        g_chunkTotalLen    = 0;
        g_chunkExpectedSeq = 0;
    }
    else
    {
        g_chunkExpectedSeq++;
    }

    rsp[0] = kChunkCcOK;
    rsp[1] = g_chunkExpectedSeq;
    *data_len = 2;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0xA1 — AMI GetMdrStatus (1-byte status query) ───────────────
// Returns [CC, nextExpectedSeq, accumLen_LE, totalLen_LE] so BIOS can verify
// where it is in the chunked stream before sending the next 0xA0.
static ipmi_ret_t handlerAmiGetMdrStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0xA1 GetMdrStatus REQ[%zu]=%s nextSeq=%u accum=%u total=%u",
             *data_len, hexDump(req, *data_len).c_str(),
             g_chunkExpectedSeq, g_chunkAccumLen, g_chunkTotalLen);

    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = kChunkCcOK;
    rsp[1] = g_chunkExpectedSeq;
    rsp[2] = g_chunkAccumLen & 0xFF;
    rsp[3] = (g_chunkAccumLen >> 8) & 0xFF;
    rsp[4] = g_chunkTotalLen & 0xFF;
    rsp[5] = (g_chunkTotalLen >> 8) & 0xFF;
    *data_len = 6;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0xF3 — AMI GetStatus (1-byte poll) ──────────────────────────
// BIOS polls this after sending chunks, up to 5 retries with 1s sleep, waiting
// for BMC to report completion. By this point the streamed chunks are already
// on disk (see handlerAmiSetSmbios) — this handler just finalizes by triggering
// the smbios-mdrv2 AgentSynchronizeData D-Bus call so the daemon reparses the
// new table.
static ipmi_ret_t handlerAmiGetStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0xF3 GetStatus REQ[%zu]=%s state=%d buf=%zu real=%d",
             *data_len, hexDump(req, *data_len).c_str(),
             (int)g_state, g_smbiosBuf.size(),
             g_hasRealSmbios ? 1 : 0);

    // 0xB5 is passive (baked dmp is authoritative); 0xF3 just acks readiness.

    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x00;  // ready/done
    *data_len = 1;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x5D — Legacy two-phase Begin/End ───────────────────────────
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
        // AMI Aptio BIOS expects a 1-byte completion status for BOTH phases.
        // Without it the host-side KCS driver waits ~5s for the payload,
        // times out, and falls back to 0x72 directly — desynchronizing the
        // session and breaking the directory exchange.
        static_cast<uint8_t*>(response)[0] = 0x00;
        *data_len = 1;
    }
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x30 — Agent Status ─────────────────────────────────────────
// dataRequest = 1 tells BIOS "BMC wants you to push SMBIOS now".
static ipmi_ret_t handlerMdrAgentStatus(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x30 AgentStatus REQ[%zu]=%s real=%d",
             *data_len, hexDump(req, *data_len).c_str(),
             g_hasRealSmbios ? 1 : 0);
    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;  // mdrVersion
    rsp[1] = 0x01;  // agentVersion
    rsp[2] = 0x00;  // dirVersion
    rsp[3] = 0x02;  // dirEntries — must match handlerMdrGetDir's entryCount.
                    // BIOS cross-checks these; mismatch flags the payload as
                    // inconsistent and aborts the directory exchange.
    // dataRequest: 1 = "BMC wants you to push SMBIOS now"; gated on
    // g_hasRealSmbios so the skeleton doesn't suppress the push request.
    rsp[4] = g_hasRealSmbios ? 0x00 : 0x01;
    *data_len = 5;
    return IPMI_CC_OK;
}

// ── Handler: Cmd 0x31 — Get MDR Directory ────────────────────────────────────
// Response:
//   [dirVersion, entryCount, remaining, entry0(16), entry1(16)]
// Each 16-byte entry describes one region:
//   [regionId, validFlag, sizeLSB, sizeMSB, usedLSB, usedMSB,
//    maxLSB, maxMSB, checksumLSB, reserved×7]
// Region 0 = SMBIOS structure table (size reflects live cache).
// Region 1 = SMBIOS entry-point anchor (always 31 bytes).
static ipmi_ret_t handlerMdrGetDir(
    ipmi_netfn_t, ipmi_cmd_t,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("Cmd 0x31 GetDir REQ[%zu]=%s valid=%d size=%u",
             *data_len, hexDump(req, *data_len).c_str(),
             g_dataValidThisSession ? 1 : 0, g_dataSize);

    uint8_t* rsp = static_cast<uint8_t*>(response);
    rsp[0] = 0x01;  // dirVersion
    rsp[1] = 0x02;  // entryCount = 2
    rsp[2] = 0x00;  // remaining

    auto writeEntry = [](uint8_t* out, uint8_t regionId, bool valid,
                         uint16_t size, uint16_t maxSize, uint8_t checksum) {
        std::memset(out, 0, 16);
        out[0]  = regionId;
        out[1]  = valid ? 0x01 : 0x00;
        out[2]  = size & 0xFF;
        out[3]  = (size >> 8) & 0xFF;
        out[4]  = size & 0xFF;        // used == size
        out[5]  = (size >> 8) & 0xFF;
        out[6]  = maxSize & 0xFF;
        out[7]  = (maxSize >> 8) & 0xFF;
        out[8]  = checksum;
        // out[9..15] reserved zeros
    };

    // Entry 0: SMBIOS structure table — only advertise as valid when we
    // actually have a real table. Skeleton presence doesn't count, or BIOS
    // won't push.
    writeEntry(rsp + 3, kRegionSmbios,
               g_hasRealSmbios,
               g_hasRealSmbios ? g_dataSize : 0,
               /*maxSize*/ 0xFFFF,
               g_hasRealSmbios ? static_cast<uint8_t>(g_dataChecksum & 0xFF) : 0);

    // Entry 1: SMBIOS entry-point anchor — always 31 bytes, always valid.
    writeEntry(rsp + 3 + 16, kRegionMeta,
               /*valid*/ true, /*size*/ 31, /*maxSize*/ 31,
               /*checksum*/ 0);

    *data_len = 3 + 16 + 16;
    return IPMI_CC_OK;
}

// ── Generic probe — logs unknown commands for protocol discovery ─────────────
// CRITICAL: return CC_OK with a 1-byte success payload, NOT an error code.
// AMI BIOS interprets any error response on its sequence as fatal and aborts
// the multi-step push (verified live: 0xAB returning IPMI_CC_INVALID_FIELD_REQUEST
// caused BIOS to abandon the 0xB5 stream after only 24 bytes).
static ipmi_ret_t handlerProbe(
    ipmi_netfn_t netfn, ipmi_cmd_t cmd,
    ipmi_request_t request, ipmi_response_t response,
    ipmi_data_len_t data_len, ipmi_context_t)
{
    const uint8_t* req = static_cast<const uint8_t*>(request);
    LOG_INFO("PROBE NetFn=0x%02X Cmd=0x%02X REQ[%zu]=%s",
             netfn, cmd, *data_len, hexDump(req, *data_len).c_str());
    static_cast<uint8_t*>(response)[0] = 0x00;  // generic success status
    *data_len = 1;
    return IPMI_CC_OK;
}

// ── Registration ──────────────────────────────────────────────────────────────
void setupGlobalOemFunctions() __attribute__((constructor));
void setupGlobalOemFunctions()
{
    LOG_INFO("registering AMI MDR handlers (v22: chunked 0xA0 + 0xB2 → Type 0 overlay)");

    primeFromCache();

    for (auto nf : {netFnAmi32, netFnAmi3A})
    {
        // Agent / Directory
        ipmi_register_callback(nf, cmdMdrAgentStatus,  nullptr, handlerMdrAgentStatus,  PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrGetDir,       nullptr, handlerMdrGetDir,       PRIVILEGE_ADMIN);

        // Write path
        ipmi_register_callback(nf, cmdMdrGetStatus,    nullptr, handlerMdrGetStatus,    PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteBegin,   nullptr, handlerMdrWriteBegin,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteChunk,   nullptr, handlerMdrWriteChunk,   PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrWriteEnd,     nullptr, handlerMdrWriteEnd,     PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrSendDir,      nullptr, handlerMdrLegacyCtrl,   PRIVILEGE_ADMIN);

        // Read path
        ipmi_register_callback(nf, cmdMdrRegionStatus, nullptr, handlerMdrRegionStatus, PRIVILEGE_ADMIN);
        ipmi_register_callback(nf, cmdMdrGetBlock,     nullptr, handlerMdrGetBlock,     PRIVILEGE_ADMIN);

        // Real AMI BIOS push path (NetFn 0x3A only — reverse-engineered)
        if (nf == netFnAmi3A)
        {
            ipmi_register_callback(nf, 0xA0,              nullptr, handlerAmiSetMdrPos,    PRIVILEGE_ADMIN);
            ipmi_register_callback(nf, 0xA1,              nullptr, handlerAmiGetMdrStatus, PRIVILEGE_ADMIN);
            ipmi_register_callback(nf, cmdAmiSetBiosInfo, nullptr, handlerAmiSetBiosInfo,  PRIVILEGE_ADMIN);
            ipmi_register_callback(nf, cmdAmiSetSmbios,   nullptr, handlerAmiSetSmbios,    PRIVILEGE_ADMIN);
            ipmi_register_callback(nf, cmdAmiGetStatus,   nullptr, handlerAmiGetStatus,    PRIVILEGE_ADMIN);
        }

        // Probe remaining commands for protocol discovery (excludes those above).
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
            0x70, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
            0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
            // 0x80..0xFF range — probe for any other AMI commands we might
            // be missing. Excludes 0xB2/0xB5/0xF3 which are real handlers.
            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
            0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
            0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
            0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
            /* 0xA0 real */ /* 0xA1 real */
            0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9,
            0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
            0xB0, 0xB1, /* 0xB2 real */ 0xB3, 0xB4, /* 0xB5 real */
            0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
            0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
            0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
            0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
            0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
            0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
            0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
            0xF0, 0xF1, 0xF2, /* 0xF3 real */ 0xF4, 0xF5, 0xF6, 0xF7,
            0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, /* 0xFF reserved */
        };
        for (uint8_t c : probeCmds)
            ipmi_register_callback(nf, c, nullptr, handlerProbe, PRIVILEGE_ADMIN);
    }

    LOG_INFO("AMI MDR handlers registered");
}