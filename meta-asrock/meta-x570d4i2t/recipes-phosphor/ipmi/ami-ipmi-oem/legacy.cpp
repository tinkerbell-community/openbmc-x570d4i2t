// SPDX-License-Identifier: Apache-2.0
//
// AMI legacy + MDR V2 endpoints on NetFn 0x3A/0x32 for ASRock X570D4I-2T.
//
// Two surfaces backed by the converter (AMI Aptio V <-> MDR V2):
//
//   * AMI proprietary push (BIOS-side):
//       0xF3 GetStatus, 0xB2 SetBiosInfo, 0xA0 SetMdrPos,
//       0xA1 GetMdrStatus, 0xB5 SetSmbiosChunk
//     0xB5 chunks (seq, flag, len LE, data) accumulate into a buffer; on End
//     the buffer is fed to ami::persistAmiBuffer() which strips the leading
//     AMI MDR_DATA_HDR (if present) and writes /var/lib/smbios/smbios2.
//
//   * MDR V2 (clients reading the AMI view over IPMI):
//       0x30 AgentStatus, 0x31 GetDir, 0x3D GetStatus,
//       0x51 WriteBegin, 0x52 WriteChunk, 0x53 WriteEnd,
//       0x71 RegionStatus, 0x72 GetBlock
//     Reads serve the AMI Aptio V view (AmiMdrHeader + payload). Writes
//     accumulate AMI-format bytes and persist via the same converter on End.
//
// Modeled on the modern phosphor-host-ipmid registration pattern
// (ipmi::registerHandler with ipmi::RspType returns and auto-deserialized
// arguments). Handlers are stateless free functions sharing global stream
// state guarded by phosphor-ipmi-host's serial dispatcher.

#include "converter.hpp"

#include <ipmid/api.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

#define LOG_INFO(fmt, ...)                                                     \
  fprintf(stderr, "ami-legacy [INFO]: " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)                                                     \
  fprintf(stderr, "ami-legacy [WARN]: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)                                                      \
  fprintf(stderr, "ami-legacy [ERR]:  " fmt "\n", ##__VA_ARGS__)

constexpr ipmi::Cmd kCmdMdrAgentStatus = 0x30;
constexpr ipmi::Cmd kCmdMdrGetDir = 0x31;
constexpr ipmi::Cmd kCmdMdrGetStatus = 0x3D;
constexpr ipmi::Cmd kCmdMdrWriteBegin = 0x51;
constexpr ipmi::Cmd kCmdMdrWriteChunk = 0x52;
constexpr ipmi::Cmd kCmdMdrWriteEnd = 0x53;
constexpr ipmi::Cmd kCmdMdrRegionStatus = 0x71;
constexpr ipmi::Cmd kCmdMdrGetBlock = 0x72;

constexpr ipmi::Cmd kCmdAmiGetStatus = 0xF3;
constexpr ipmi::Cmd kCmdAmiSetBiosInfo = 0xB2;
// 0xA0/0xA1 in this BIOS are actually BackupBmcMacDxe operations
// (request body = [LAN_channel:1, mac:6]). We still treat the first byte as
// "region" and the next two as "offset" so the slot queue can position
// 0xB5 chunks — see the protocol note above the state declarations.
constexpr ipmi::Cmd kCmdAmiSetMdrPos = 0xA0;
constexpr ipmi::Cmd kCmdAmiGetMdrStatus = 0xA1;
constexpr ipmi::Cmd kCmdAmiSetChunk = 0xB5;

constexpr uint8_t kRegionSmbios = 0;
constexpr uint8_t kRegionMeta = 1;

constexpr size_t kMaxReadChunk = 4096;

// 0xB5 chunk-flag bits per X570D4I-2T BIOS spec.
constexpr uint8_t kChunkFlagStart = 0x01;
constexpr uint8_t kChunkFlagContinue = 0x02;
constexpr uint8_t kChunkFlagEnd = 0x04;

// OEM completion codes for 0xB5.
constexpr ipmi::Cc kCcSeqMismatch = 0xC7;
constexpr ipmi::Cc kCcLengthMismatch = 0xC8;

// ── MDR V2 write-machine state ──────────────────────────────────────────────
enum class MdrWriteState : uint8_t { Idle, Open, Receiving };
MdrWriteState g_mdrState = MdrWriteState::Idle;
uint32_t g_mdrDeclaredSize = 0;
std::vector<uint8_t> g_mdrBuf;

// ── 0xA0 / 0xA1 + 0xB5 state ────────────────────────────────────────────────
//
// PROTOCOL NOTE — from RE of the BIOS firmware:
//   * 0xA0 in this BIOS is sent by BackupBmcMacDxe with body
//     [ LAN_channel:1, mac:6 ] — its real purpose is MAC backup, NOT
//     SMBIOS positioning.
//   * 0xA1 is the read-back of that MAC.
//   * 0xB5 is sent by SendInfoBmcIpmiDxe (and ChassisIdDxe) with body
//     [ 0x00 reserved, ASCII string..., 0x00 NUL ] — no destination metadata.
//
// We still parse the 0xA0 request as (region:1, offset:2 LE) and feed those
// values into a FIFO slot queue consumed by 0xB5. Effectively that means the
// "offset" we use to position 0xB5 chunks is the first two bytes of the
// BMC MAC address (LE). Logged plainly so the behaviour is auditable, and
// the queue is still in place if a future BIOS revision actually does ship
// a real SetMdrPos sender.
struct AmiSlot {
  uint8_t region;
  uint32_t offset;
};
std::deque<AmiSlot> g_amiSlots;
constexpr size_t kMaxAmiSlots = 1024;

uint8_t g_amiRegion = 0;
uint32_t g_amiCursor = 0;
uint32_t g_amiStringIndex = 0;
std::vector<std::string> g_amiPushedStrings;

// In-memory working copy of the SMBIOS payload (raw bytes, no MDR or AMI
// header). Initialised at library-load time from the converter's view of
// /var/lib/smbios/smbios2.
std::vector<uint8_t> g_amiBuf;

// Per-region read cursor for 0x72 MdrGetBlock. The command behaves like TFTP:
// each successful call returns the next kMaxReadChunk bytes and advances the
// cursor by that many. An incoming offset of 0 resets the cursor (start of a
// new transfer). Reaching end-of-stream returns an empty chunk and resets the
// cursor so the next offset=0 starts cleanly.
constexpr size_t kNumRegions = 4;
uint32_t g_mdrReadCursor[kNumRegions] = {0, 0, 0, 0};

// Hex-dump up to `maxBytes` of `data` as "AA BB CC ..." into a heap string.
std::string hexDump(const std::vector<uint8_t> &data, size_t maxBytes = 64) {
  size_t n = std::min(data.size(), maxBytes);
  std::string out;
  out.reserve(n * 3 + 8);
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    snprintf(buf, sizeof(buf), "%02X ", data[i]);
    out.append(buf);
  }
  if (data.size() > maxBytes) {
    out.append("...");
  }
  return out;
}

// Render printable ASCII slice for the same window (non-printables → '.').
std::string asciiSlice(const std::vector<uint8_t> &data, size_t maxBytes = 64) {
  size_t n = std::min(data.size(), maxBytes);
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = data[i];
    out.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
  }
  return out;
}

// Load the on-disk MDR V2 file into the in-memory working buffer. Called
// once at library load and any time we want to discard pending overlays.
void loadWorkingBuffer() {
  g_amiBuf = ami::loadMdrPayload();
  LOG_INFO("loadWorkingBuffer: %zu bytes from %s", g_amiBuf.size(),
           ami::kSmbiosFile);
}

// Persist the in-memory working buffer back to the on-disk MDR V2 file and
// trigger smbios-mdrv2 to re-publish. Called after every region-0 overlay.
void persistWorkingBuffer() {
  if (g_amiBuf.empty()) {
    LOG_WARN("persistWorkingBuffer: empty buffer — skipping");
    return;
  }
  if (!ami::writeMdrFile(g_amiBuf.data(), g_amiBuf.size())) {
    LOG_ERR("persistWorkingBuffer: writeMdrFile failed");
    return;
  }
  if (!ami::triggerMdrSync()) {
    LOG_WARN("persistWorkingBuffer: AgentSynchronizeData failed (file "
             "written, sync skipped)");
    return;
  }
  LOG_INFO("persistWorkingBuffer: %zu bytes written + synced",
           g_amiBuf.size());
}

void commitMdrBuffer() {
  if (g_mdrBuf.empty()) {
    g_mdrState = MdrWriteState::Idle;
    g_mdrDeclaredSize = 0;
    return;
  }
  if (ami::persistAmiBuffer(g_mdrBuf.data(), g_mdrBuf.size())) {
    ami::triggerMdrSync();
    LOG_INFO("MDR V2 write committed (%zu bytes)", g_mdrBuf.size());
  } else {
    LOG_ERR("MDR V2 write commit failed");
  }
  g_mdrBuf.clear();
  g_mdrState = MdrWriteState::Idle;
  g_mdrDeclaredSize = 0;
}

void commitAmiBuffer() {
  if (g_amiBuf.empty()) {
    return;
  }
  if (g_amiRegion == kRegionSmbios) {
    if (ami::persistAmiBuffer(g_amiBuf.data(), g_amiBuf.size())) {
      ami::triggerMdrSync();
      LOG_INFO("AMI push committed (%zu bytes)", g_amiBuf.size());
    } else {
      LOG_ERR("AMI push commit failed");
    }
  } else {
    LOG_INFO("AMI push for region %u (%zu bytes) — not persisted", g_amiRegion,
             g_amiBuf.size());
  }
  g_amiBuf.clear();
  g_amiCursor = 0;
}

// ────────────────────────────────────────────────────────────────────────────
// MDR V2 handlers
// ────────────────────────────────────────────────────────────────────────────

ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t>
hMdrGetStatus(ipmi::Context::ptr) {
  LOG_INFO("Cmd 0x3D MdrGetStatus → 00 00 10 01");
  return ipmi::responseSuccess(uint8_t{0x00}, uint8_t{0x00}, uint8_t{0x10},
                               uint8_t{0x01});
}

ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>
hMdrAgentStatus(ipmi::Context::ptr) {
  uint8_t dataInit = g_amiBuf.empty() ? 0x01 : 0x00;
  LOG_INFO("Cmd 0x30 MdrAgentStatus → 01 01 00 01 %02X (buf=%zu)", dataInit,
           g_amiBuf.size());
  return ipmi::responseSuccess(uint8_t{0x01}, uint8_t{0x01}, uint8_t{0x00},
                               uint8_t{0x01}, dataInit);
}

ipmi::RspType<std::vector<uint8_t>> hMdrGetDir(ipmi::Context::ptr) {
  LOG_INFO("Cmd 0x31 MdrGetDir (buf=%zu)", g_amiBuf.size());
  auto view = ami::amiViewFromCache();
  uint16_t size = static_cast<uint16_t>(view.size());
  uint16_t chk = ami::computeChecksum(view.data(), view.size());
  bool valid = !view.empty();

  std::vector<uint8_t> rsp(3 + 16 + 16, 0);
  rsp[0] = 0x01;
  rsp[1] = 0x02;
  rsp[2] = 0x00;

  auto writeEntry = [](uint8_t *out, uint8_t regionId, bool valid,
                       uint16_t size, uint16_t maxSize, uint8_t checksum) {
    out[0] = regionId;
    out[1] = valid ? 0x01 : 0x00;
    out[2] = size & 0xFF;
    out[3] = (size >> 8) & 0xFF;
    out[4] = size & 0xFF;
    out[5] = (size >> 8) & 0xFF;
    out[6] = maxSize & 0xFF;
    out[7] = (maxSize >> 8) & 0xFF;
    out[8] = checksum;
  };
  writeEntry(rsp.data() + 3, kRegionSmbios, valid, size, 0xFFFF,
             static_cast<uint8_t>(chk & 0xFF));
  writeEntry(rsp.data() + 3 + 16, kRegionMeta, true, sizeof(ami::AmiMdrHeader),
             sizeof(ami::AmiMdrHeader), 0);
  return ipmi::responseSuccess(rsp);
}

ipmi::RspType<std::vector<uint8_t>> hMdrRegionStatus(ipmi::Context::ptr,
                                                     uint8_t regionId) {
  LOG_INFO("Cmd 0x71 MdrRegionStatus region=%u (buf=%zu, mdrState=%s)",
           regionId, g_amiBuf.size(),
           g_mdrState == MdrWriteState::Idle ? "Idle"
           : g_mdrState == MdrWriteState::Open ? "Open" : "Receiving");
  auto view = ami::amiViewFromCache();
  bool valid = !view.empty();
  uint16_t size = valid ? static_cast<uint16_t>(view.size()) : 0;
  uint16_t chk = valid ? ami::computeChecksum(view.data(), view.size()) : 0;

  std::vector<uint8_t> rsp(10, 0);
  rsp[0] = 0x01;
  rsp[1] = regionId;
  rsp[2] = valid ? 0x01 : 0x00;
  rsp[3] = (g_mdrState != MdrWriteState::Idle) ? 0x01 : 0x00;
  rsp[4] = 0x00;
  rsp[5] = size & 0xFF;
  rsp[6] = (size >> 8) & 0xFF;
  rsp[7] = size & 0xFF;
  rsp[8] = (size >> 8) & 0xFF;
  rsp[9] = chk & 0xFF;
  return ipmi::responseSuccess(rsp);
}

// 0x72 MdrGetBlock — TFTP-style sequential reader.
//
// The client passes (region, offset). offset == 0 is treated as "start a new
// transfer" and resets our per-region cursor. Any other offset is honoured if
// it equals the current cursor; mismatches log a warning and seek the cursor
// to the requested position. Each call returns the next kMaxReadChunk bytes
// from the (possibly seek-adjusted) cursor and advances the cursor by that
// many bytes. End-of-stream returns an empty payload and resets the cursor.
ipmi::RspType<std::vector<uint8_t>>
hMdrGetBlock(ipmi::Context::ptr, uint8_t regionId, uint16_t offset) {
  if (regionId >= kNumRegions) {
    LOG_WARN("Cmd 0x72 MdrGetBlock unknown region=%u — empty response",
             regionId);
    return ipmi::responseSuccess(std::vector<uint8_t>{});
  }

  auto view = ami::amiViewFromCache();
  if (regionId == kRegionMeta) {
    // Region 1 exposes just the 4-byte AmiMdrHeader prefix.
    view.resize(std::min(view.size(), sizeof(ami::AmiMdrHeader)));
  }

  uint32_t& cursor = g_mdrReadCursor[regionId];

  // offset==0 always starts a new transfer.
  if (offset == 0) {
    cursor = 0;
  } else if (offset != cursor) {
    LOG_WARN("Cmd 0x72 MdrGetBlock region=%u offset=0x%04X mismatches "
             "cursor=0x%04X — seeking to requested offset",
             regionId, offset, cursor);
    cursor = offset;
  }

  if (cursor >= view.size()) {
    LOG_INFO("Cmd 0x72 MdrGetBlock region=%u end-of-stream "
             "(cursor=0x%04X size=%zu) — cursor reset",
             regionId, cursor, view.size());
    cursor = 0;
    return ipmi::responseSuccess(std::vector<uint8_t>{});
  }

  size_t sendLen = std::min(view.size() - cursor, kMaxReadChunk*1000);
  std::vector<uint8_t> chunk(view.begin() + cursor,
                             view.begin() + cursor + sendLen);
  uint32_t before = cursor;
  cursor += static_cast<uint32_t>(sendLen);
  LOG_INFO("Cmd 0x72 MdrGetBlock region=%u offset=0x%04X → %zu bytes "
           "(cursor 0x%04X → 0x%04X, view=%zu)",
           regionId, before, sendLen, before, cursor, view.size());
  return ipmi::responseSuccess(chunk);
}

ipmi::RspType<> hMdrWriteBegin(ipmi::Context::ptr, uint8_t regionId,
                               uint16_t declaredSize) {
  LOG_INFO("Cmd 0x51 MdrWriteBegin region=%u declaredSize=%u", regionId,
           declaredSize);
  g_mdrBuf.clear();
  g_mdrBuf.reserve(declaredSize);
  g_mdrDeclaredSize = declaredSize;
  g_mdrState = MdrWriteState::Open;
  return ipmi::responseSuccess();
}

ipmi::RspType<> hMdrWriteChunk(ipmi::Context::ptr, uint8_t regionId,
                               uint16_t offset, std::vector<uint8_t> payload) {
  LOG_INFO("Cmd 0x52 MdrWriteChunk region=%u offset=0x%04X len=%zu",
           regionId, offset, payload.size());
  size_t needed = static_cast<size_t>(offset) + payload.size();
  if (needed > ami::kMaxPayload + sizeof(ami::AmiMdrHeader)) {
    LOG_ERR("0x52 chunk would exceed buffer cap (offset=%u + len=%zu)",
            offset, payload.size());
    return ipmi::response(ipmi::ccReqDataLenInvalid);
  }
  if (needed > g_mdrBuf.size()) {
    g_mdrBuf.resize(needed, 0);
  }
  std::copy(payload.begin(), payload.end(), g_mdrBuf.begin() + offset);
  g_mdrState = MdrWriteState::Receiving;
  return ipmi::responseSuccess();
}

ipmi::RspType<> hMdrWriteEnd(ipmi::Context::ptr) {
  LOG_INFO("Cmd 0x53 MdrWriteEnd (buf=%zu, declared=%u)", g_mdrBuf.size(),
           g_mdrDeclaredSize);
  commitMdrBuffer();
  return ipmi::responseSuccess();
}

// ────────────────────────────────────────────────────────────────────────────
// AMI proprietary handlers
// ────────────────────────────────────────────────────────────────────────────

ipmi::RspType<> hAmiGetStatus(ipmi::Context::ptr) {
  LOG_INFO("Cmd 0xF3 GetStatus (buf=%zu slots_queued=%zu)", g_amiBuf.size(),
           g_amiSlots.size());
  return ipmi::responseSuccess();
}

ipmi::RspType<uint8_t> hAmiGetMdrStatus(ipmi::Context::ptr) {
  LOG_INFO("Cmd 0xA1 GetMdrStatus → 0x00 (buf=%zu slots_queued=%zu)",
           g_amiBuf.size(), g_amiSlots.size());
  return ipmi::responseSuccess(uint8_t{0x00});
}

ipmi::RspType<> hAmiSetBiosInfo(ipmi::Context::ptr,
                                std::vector<uint8_t> payload) {
  char hex[3 * 32 + 4] = {0};
  size_t n = std::min<size_t>(payload.size(), 16);
  for (size_t i = 0; i < n; ++i) {
    snprintf(hex + i * 3, 4, "%02X ", payload[i]);
  }
  LOG_INFO("Cmd 0xB2 SetBiosInfo (%zu bytes): %s", payload.size(), hex);
  return ipmi::responseSuccess();
}

ipmi::RspType<> hAmiSetMdrPos(ipmi::Context::ptr, uint8_t region,
                              uint16_t offset) {
  // Enqueue this destination. The BIOS sends all SetMdrPos calls before any
  // SetSmbiosChunk calls, so we need to remember every (region, offset) so
  // the corresponding 0xB5 chunks can be placed correctly.
  if (g_amiSlots.size() < kMaxAmiSlots) {
    g_amiSlots.push_back({region, offset});
  } else {
    LOG_WARN("0xA0 slot queue full (%zu) — dropping region=%u offset=0x%04X",
             g_amiSlots.size(), region, offset);
  }
  g_amiRegion = region;
  g_amiCursor = offset;
  LOG_INFO("Cmd 0xA0 SetMdrPos region=%u offset=0x%04X (queued, depth=%zu) "
           "[NOTE: BIOS RE shows this is actually BackupBmcMac LAN_ch + MAC]",
           region, offset, g_amiSlots.size());
  return ipmi::responseSuccess();
}

// 0xB5 SetSmbiosChunk — wire format confirmed via BIOS RE.
//
// Reverse-engineered from the ASRock X570D4I-2T AMI Aptio V module
// `SendInfoBmcIpmiDxe` (GUID 9df02dfd-8cf7-4fc7-b8ae-cbd9560a3f24). The BIOS
// builds the request body as:
//
//     [ reserved:1=0x00 ][ string_bytes... ][ trailing NUL:1=0x00 ]
//
// There is NO seq, NO flag, and NO length field. Each 0xB5 call pushes ONE
// null-terminated SMBIOS string. The destination (region + offset within the
// SMBIOS table) is taken from the most recent 0xA0 SetMdrPos. The BIOS only
// inspects the EFI_STATUS return from EFI_IPMI_PROTOCOL.SubmitCommand — it
// does NOT validate the response body — so any successful CC is accepted.
// On EFI_STATUS error the BIOS Stall()s ~1000us and retries up to 5 times,
// then moves on.
//
// We use the leading reserved byte as a self-check (warn if non-zero), strip
// it and the trailing NUL, then overlay the string bytes at g_amiCursor in
// g_amiBuf — building up our own picture of what the BIOS wants the final
// SMBIOS table to contain. Persistence is left for a follow-up once the
// overlay set is understood; this handler is observation + accumulation only.
ipmi::RspType<> hAmiSetChunk(ipmi::Context::ptr,
                             std::vector<uint8_t> raw) {
  if (raw.empty()) {
    LOG_WARN("0xB5 empty request body");
    return ipmi::responseSuccess();
  }
  if (raw[0] != 0x00) {
    LOG_WARN("0xB5 reserved byte not 0x00 (got 0x%02X) — layout may differ",
             raw[0]);
  }

  // Strip leading reserved byte and trailing NUL (if present) to recover the
  // raw string the BIOS is overlaying.
  size_t startIdx = 1;
  size_t endIdx   = raw.size();
  if (endIdx > startIdx && raw[endIdx - 1] == 0x00) {
    endIdx -= 1;
  }
  std::vector<uint8_t> payload(raw.begin() + startIdx, raw.begin() + endIdx);

  // Dequeue the next (region, offset) slot. If we've run out, fall back to
  // the last-seen cursor (advancing forward) so the BIOS still gets ACKs.
  AmiSlot slot;
  if (!g_amiSlots.empty()) {
    slot = g_amiSlots.front();
    g_amiSlots.pop_front();
  } else {
    LOG_WARN("0xB5 with empty slot queue — using last cursor=0x%04X",
             g_amiCursor);
    slot = {g_amiRegion, g_amiCursor};
  }

  LOG_INFO("0xB5 string at region=%u offset=0x%04X len=%zu: \"%s\" "
           "(slots_left=%zu)",
           slot.region, slot.offset, payload.size(),
           asciiSlice(payload, 256).c_str(), g_amiSlots.size());
  LOG_INFO("0xB5     hex: %s", hexDump(payload, 96).c_str());

  // Overlay only for region 0 (SMBIOS structure table). Region 1 destinations
  // (the BIOS uses e.g. offset 0x6B9C, well beyond the structure table) are
  // logged but not applied — they map to a BIOS-side address space we don't
  // currently mirror.
  if (slot.region == kRegionSmbios) {
    size_t end = static_cast<size_t>(slot.offset) + payload.size();
    if (end <= ami::kMaxPayload) {
      if (end > g_amiBuf.size()) {
        g_amiBuf.resize(end, 0);
      }
      std::copy(payload.begin(), payload.end(),
                g_amiBuf.begin() + slot.offset);
      persistWorkingBuffer();
    } else {
      LOG_ERR("0xB5 overlay would exceed buffer cap (offset=0x%04X len=%zu)",
              slot.offset, payload.size());
    }
  } else {
    LOG_INFO("0xB5 region=%u overlay skipped (only region 0 is mirrored)",
             slot.region);
  }

  // Update last-seen cursor in case we underrun later.
  g_amiRegion = slot.region;
  g_amiCursor = slot.offset + static_cast<uint32_t>(payload.size());

  // Bare CC=OK. BIOS ignores response body; this is the most permissive ACK.
  return ipmi::responseSuccess();
}

} // namespace

void setupAmiLegacyHandlers() __attribute__((constructor));
void setupAmiLegacyHandlers() {
  LOG_INFO("registering AMI legacy + MDR V2 handlers on NetFn 0x3A/0x32");
  ami::seedFromBakedIfMissing();
  loadWorkingBuffer();

  for (ipmi::NetFn nf : {ipmi::netFnOemTwo, ipmi::netFnOemSix}) {
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrAgentStatus,
                          ipmi::Privilege::Admin, hMdrAgentStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrGetDir,
                          ipmi::Privilege::Admin, hMdrGetDir);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrGetStatus,
                          ipmi::Privilege::Admin, hMdrGetStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrWriteBegin,
                          ipmi::Privilege::Admin, hMdrWriteBegin);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrWriteChunk,
                          ipmi::Privilege::Admin, hMdrWriteChunk);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrWriteEnd,
                          ipmi::Privilege::Admin, hMdrWriteEnd);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrRegionStatus,
                          ipmi::Privilege::Admin, hMdrRegionStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrGetBlock,
                          ipmi::Privilege::Admin, hMdrGetBlock);

    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdAmiGetStatus,
                          ipmi::Privilege::Admin, hAmiGetStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdAmiSetBiosInfo,
                          ipmi::Privilege::Admin, hAmiSetBiosInfo);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdAmiGetMdrStatus,
                          ipmi::Privilege::Admin, hAmiGetMdrStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdAmiSetMdrPos,
                          ipmi::Privilege::Admin, hAmiSetMdrPos);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdAmiSetChunk,
                          ipmi::Privilege::Admin, hAmiSetChunk);
  }
  LOG_INFO("AMI legacy + MDR V2 handlers ready");
}
