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

// 0xB5 SetSmbiosChunk call index — increments with each 0xB5 received and is
// used to look up which SMBIOS string slot to write into. The BIOS sends the
// same identifier ("X570D4I-2T") multiple times in succession to populate
// several related SMBIOS string slots; the call index lets us route each to
// the correct (Type, string#) slot.
struct StringSlot {
  uint8_t  smbiosType;
  uint8_t  stringIndex;   // 1-based
  const char* label;
};
constexpr StringSlot kAmiSlotTable[] = {
  { 1, 1, "Type1.SystemManufacturer" },
  { 1, 2, "Type1.SystemProductName" },
  { 2, 1, "Type2.BoardManufacturer" },
  { 2, 2, "Type2.BoardProductName" },
};
constexpr size_t kNumAmiSlots =
    sizeof(kAmiSlotTable) / sizeof(kAmiSlotTable[0]);
size_t g_amiCallIndex = 0;

// In-memory working copy of the SMBIOS payload (raw bytes, no MDR or AMI
// header). Initialised at library-load time from the converter's view of
// /var/lib/smbios/smbios2.
std::vector<uint8_t> g_amiBuf;

// Number of distinct regions we route 0x71/0x72 against.
constexpr size_t kNumRegions = 4;

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

// Replace string N (1-based) inside the SMBIOS structure of `type` in `buf`
// with `newStr`. The string area is the variable-length region after the
// formatted area (offset +Length) and ending at the double-NUL terminator.
// Resizes `buf` in place when the new string differs in length.
// Returns true on success.
bool replaceSmbiosString(std::vector<uint8_t>& buf, uint8_t type,
                          uint8_t stringIndex,
                          const std::string& newStr);

// Find the byte offset of the first SMBIOS structure of `type` in `buf`.
// Returns SIZE_MAX if not found.
//
// SMBIOS layout inside `buf` (the raw payload, no MDR/AMI header):
//   bytes 0..23: _SM3_ anchor (24 bytes for SMBIOS 3.0; older _SM_ is 31)
//   bytes 24..:  structure table: each entry is
//                  [ Type:1, Length:1, Handle:2, formatted fields..., strings, 0x00, 0x00 ]
//                Length covers the formatted portion; the trailing strings end
//                with a double-NUL terminator.
size_t findSmbiosStructOffset(const std::vector<uint8_t>& buf, uint8_t type) {
  size_t pos = 0;
  if (buf.size() >= 5 && buf[0] == '_' && buf[1] == 'S' && buf[2] == 'M' &&
      buf[3] == '3' && buf[4] == '_') {
    pos = 24;  // _SM3_ anchor is 24 bytes
  } else if (buf.size() >= 4 && buf[0] == '_' && buf[1] == 'S' &&
             buf[2] == 'M' && buf[3] == '_') {
    pos = 31;  // _SM_ anchor is 31 bytes
  }
  while (pos + 4 <= buf.size()) {
    uint8_t structType = buf[pos];
    uint8_t structLen  = buf[pos + 1];
    if (structLen < 4) {
      break;
    }
    if (structType == type) {
      return pos;
    }
    if (structType == 127) {
      break;  // end-of-table marker
    }
    // Advance past formatted area + string area (ends with two NUL bytes).
    size_t end = pos + structLen;
    while (end + 1 < buf.size() && !(buf[end] == 0 && buf[end + 1] == 0)) {
      end++;
    }
    end += 2;
    if (end > buf.size()) {
      break;
    }
    pos = end;
  }
  return SIZE_MAX;
}

bool replaceSmbiosString(std::vector<uint8_t>& buf, uint8_t type,
                          uint8_t stringIndex, const std::string& newStr) {
  if (stringIndex == 0) {
    return false;
  }
  size_t structOff = findSmbiosStructOffset(buf, type);
  if (structOff == SIZE_MAX) {
    LOG_WARN("replaceSmbiosString: Type %u not found", type);
    return false;
  }
  uint8_t structLen = buf[structOff + 1];
  size_t stringAreaStart = structOff + structLen;
  if (stringAreaStart > buf.size()) {
    return false;
  }

  // Walk to string at stringIndex (1-based).
  size_t pos = stringAreaStart;
  size_t curIdx = 1;
  size_t targetStart = SIZE_MAX;
  size_t targetEnd = SIZE_MAX;
  while (pos < buf.size()) {
    if (buf[pos] == 0) {
      // Either end-of-strings (if previous byte was also 0) or null between
      // strings.
      if (pos + 1 < buf.size() && buf[pos + 1] == 0 && pos == stringAreaStart) {
        // String area is empty: no strings present, can't insert a specific
        // index.
        LOG_WARN("replaceSmbiosString: Type %u has empty string area",
                 type);
        return false;
      }
      // pos == end of current string
      if (curIdx == stringIndex) {
        targetEnd = pos;
        break;
      }
      ++curIdx;
      ++pos;
      if (pos < buf.size() && buf[pos] == 0) {
        // Reached double-NUL; index not present, append before terminator.
        targetStart = pos;
        targetEnd   = pos;
        break;
      }
      // pos now begins the next string
      if (curIdx == stringIndex) {
        targetStart = pos;
      }
      continue;
    }
    if (curIdx == stringIndex && targetStart == SIZE_MAX) {
      targetStart = pos;
    }
    ++pos;
  }
  if (curIdx == stringIndex && targetStart != SIZE_MAX &&
      targetEnd == SIZE_MAX) {
    targetEnd = pos;
  }
  if (targetStart == SIZE_MAX || targetEnd == SIZE_MAX) {
    LOG_WARN("replaceSmbiosString: Type %u string #%u not addressable "
             "(pos=%zu curIdx=%zu)",
             type, stringIndex, pos, curIdx);
    return false;
  }

  // Replace bytes [targetStart, targetEnd) with newStr.
  size_t oldLen = targetEnd - targetStart;
  size_t newLen = newStr.size();
  if (newLen == oldLen) {
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
  } else if (newLen < oldLen) {
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
    buf.erase(buf.begin() + targetStart + newLen,
              buf.begin() + targetEnd);
  } else {
    size_t growBy = newLen - oldLen;
    buf.insert(buf.begin() + targetEnd, growBy, 0);
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
  }
  return true;
}

// 0x72 MdrGetBlock — bulk reader, single-purpose per region.
//
//   region=0 (kRegionSmbios) → SMBIOS payload slice g_amiBuf[offset..]
//     g_amiBuf is the raw _SM3_ anchor + structure table with no AMI or MDR
//     header. Returns all bytes from `offset` to the end of the buffer.
//     BIOS issues successive calls with increasing offsets; an offset past the
//     end returns an empty response signalling EOF.
//
//   region=1 (kRegionMeta) → AmiMdrHeader slice hdr[offset..]
//     hdr = [ dataSize:2 LE, checksum:2 LE ] (4 bytes), built fresh from
//     g_amiBuf so size+checksum always reflect the current working buffer.
//     Returns all header bytes from `offset` to the end of the header.
ipmi::RspType<std::vector<uint8_t>>
hMdrGetBlock(ipmi::Context::ptr ctx, uint8_t regionId,
             uint16_t offset) {
  LOG_INFO("Cmd 0x72 MdrGetBlock netfn=0x%02X channel=%u region=%u "
           "offset=0x%04X (g_amiBuf=%zu)",
           ctx ? static_cast<uint8_t>(ctx->netFn) : 0,
           ctx ? static_cast<uint8_t>(ctx->channel) : 0,
           regionId, offset, g_amiBuf.size());

  if (regionId == kRegionSmbios) {
    if (offset >= g_amiBuf.size()) {
      LOG_INFO("0x72 region=0 offset=0x%04X past end (%zu) → empty",
               offset, g_amiBuf.size());
      return ipmi::responseSuccess(std::vector<uint8_t>{});
    }
    std::vector<uint8_t> slice(g_amiBuf.begin() + offset, g_amiBuf.end());
    LOG_INFO("0x72 region=0 offset=0x%04X → %zu bytes", offset, slice.size());
    return ipmi::responseSuccess(slice);
  }

  if (regionId == kRegionMeta) {
    ami::AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(g_amiBuf.size());
    hdr.checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    uint8_t hdrBytes[sizeof(hdr)];
    std::memcpy(hdrBytes, &hdr, sizeof(hdr));
    if (offset >= sizeof(hdrBytes)) {
      LOG_INFO("0x72 region=1 offset=0x%04X past end (%zu) → empty",
               offset, sizeof(hdrBytes));
      return ipmi::responseSuccess(std::vector<uint8_t>{});
    }
    std::vector<uint8_t> slice(hdrBytes + offset,
                               hdrBytes + sizeof(hdrBytes));
    LOG_INFO("0x72 region=1 offset=0x%04X → %zu bytes (AmiMdrHeader)",
             offset, slice.size());
    return ipmi::responseSuccess(slice);
  }

  LOG_WARN("0x72 unknown region=%u — empty response", regionId);
  return ipmi::responseSuccess(std::vector<uint8_t>{});
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

// 0xA1 GetMdrStatus — AMI legacy region-status probe sent by the BIOS after
// each push to verify the BMC accepted the data. The request is one byte
// (the region ID); the response carries the current size + checksum of that
// region so the BIOS can compare against what it just pushed.
//
// Response layout (matches AMI legacy region-status pattern):
//     [ regionId:1, valid:1, dataSize:2 LE, checksum:2 LE ] = 6 bytes
//
//   region=0 → working SMBIOS payload (g_amiBuf)
//   region=1 → 4-byte AmiMdrHeader describing the working payload
ipmi::RspType<std::vector<uint8_t>>
hAmiGetMdrStatus(ipmi::Context::ptr ctx, std::vector<uint8_t> req) {
  uint8_t regionId = req.empty() ? 0 : req[0];

  uint16_t dataSize = 0;
  uint16_t checksum = 0;
  uint8_t  valid    = 0;
  if (regionId == kRegionSmbios) {
    dataSize = static_cast<uint16_t>(g_amiBuf.size());
    checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    valid    = g_amiBuf.empty() ? 0 : 1;
  } else if (regionId == kRegionMeta) {
    ami::AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(g_amiBuf.size());
    hdr.checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    dataSize = sizeof(hdr);
    uint8_t hdrBytes[sizeof(hdr)];
    std::memcpy(hdrBytes, &hdr, sizeof(hdr));
    checksum = ami::computeChecksum(hdrBytes, sizeof(hdrBytes));
    valid    = 1;
  } else {
    LOG_WARN("0xA1 unknown region=%u", regionId);
  }

  std::vector<uint8_t> rsp{
      regionId,
      valid,
      static_cast<uint8_t>(dataSize & 0xFF),
      static_cast<uint8_t>((dataSize >> 8) & 0xFF),
      static_cast<uint8_t>(checksum & 0xFF),
      static_cast<uint8_t>((checksum >> 8) & 0xFF),
  };

  LOG_INFO("Cmd 0xA1 GetMdrStatus netfn=0x%02X channel=%u req=[%s] "
           "→ region=%u valid=%u size=%u chk=0x%04X",
           ctx ? static_cast<uint8_t>(ctx->netFn) : 0,
           ctx ? static_cast<uint8_t>(ctx->channel) : 0,
           hexDump(req, 16).c_str(), regionId, valid, dataSize, checksum);
  return ipmi::responseSuccess(rsp);
}

// 0xB2 SetBiosInfo — BIOS pushes a 16-byte buffer that maps to SMBIOS Type 0
// (BIOS Information) formatted-area fields starting at structure offset +4
// (the 16 bytes after the standard `[Type, Length, Handle:2]` header):
//   [Vendor:1, Version:1, StartingAddrSeg:2 LE, ReleaseDate:1, RomSize:1,
//    Characteristics:8, ExtChar1:1, ExtChar2:1]
// We find Type 0 in g_amiBuf and overlay these 16 bytes at offset +4, then
// persist + sync so smbios-mdrv2 picks up the patched fields.
ipmi::RspType<> hAmiSetBiosInfo(ipmi::Context::ptr,
                                std::vector<uint8_t> payload) {
  LOG_INFO("Cmd 0xB2 SetBiosInfo (%zu bytes): %s", payload.size(),
           hexDump(payload, 32).c_str());

  if (payload.empty()) {
    LOG_WARN("0xB2 empty payload — nothing to overlay");
    return ipmi::responseSuccess();
  }

  size_t type0Off = findSmbiosStructOffset(g_amiBuf, /*type=*/0);
  if (type0Off == SIZE_MAX) {
    LOG_WARN("0xB2 Type 0 BIOS Info struct not found in g_amiBuf — overlay "
             "skipped (buf=%zu)",
             g_amiBuf.size());
    return ipmi::responseSuccess();
  }

  // Type 0 standard header is [Type:1, Length:1, Handle:2] = 4 bytes.
  // Overlay begins at the first formatted field (Vendor string index).
  size_t fieldsOff = type0Off + 4;
  uint8_t structLen = g_amiBuf[type0Off + 1];
  size_t maxOverlay = (structLen > 4) ? (structLen - 4) : 0;
  size_t overlayLen = std::min(payload.size(), maxOverlay);

  if (overlayLen == 0 ||
      fieldsOff + overlayLen > g_amiBuf.size()) {
    LOG_WARN("0xB2 overlay would not fit (type0Off=%zu structLen=%u "
             "maxOverlay=%zu buf=%zu)",
             type0Off, structLen, maxOverlay, g_amiBuf.size());
    return ipmi::responseSuccess();
  }

  std::copy(payload.begin(), payload.begin() + overlayLen,
            g_amiBuf.begin() + fieldsOff);
  LOG_INFO("0xB2 overlaid %zu bytes at Type 0 + 4 (offset=%zu, structLen=%u)",
           overlayLen, fieldsOff, structLen);

  persistWorkingBuffer();
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
ipmi::RspType<uint8_t> hAmiSetChunk(ipmi::Context::ptr,
                                    std::vector<uint8_t> raw) {
  if (raw.empty()) {
    LOG_WARN("0xB5 empty request body");
    return ipmi::responseSuccess(uint8_t{0x00});
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

  std::string str(payload.begin(), payload.end());

  // BIOS sends the same identifier multiple times in succession — each push
  // populates one SMBIOS string slot in kAmiSlotTable, cycling through the
  // table indefinitely.
  const StringSlot& target =
      kAmiSlotTable[g_amiCallIndex % kNumAmiSlots];

  LOG_INFO("0xB5 call#%zu → %s (Type %u string #%u) value=\"%s\" len=%zu",
           g_amiCallIndex, target.label, target.smbiosType,
           target.stringIndex, asciiSlice(payload, 256).c_str(),
           payload.size());

  bool changed =
      replaceSmbiosString(g_amiBuf, target.smbiosType, target.stringIndex,
                          str);
  if (changed) {
    persistWorkingBuffer();
  } else {
    LOG_WARN("0xB5 string replacement failed for %s", target.label);
  }

  ++g_amiCallIndex;
  // Keep last-cursor housekeeping so any future code that consults it sees
  // a sensible value.
  g_amiRegion = kRegionSmbios;
  g_amiCursor = static_cast<uint32_t>(payload.size());

  // AMI BMC convention for 0xB5: respond with 1 byte ack (0x00 = OK). The
  // BIOS only inspects the EFI_STATUS from SubmitCommand, not the response
  // body — but a zero-length response body trips the BIOS-side IPMI
  // transport's `ResponseSize >= 1` check on this BIOS, which returns
  // EFI_DEVICE_ERROR and triggers a retry storm. One byte of payload keeps
  // the transport happy.
  return ipmi::responseSuccess(uint8_t{0x00});
}

} // namespace

void setupAmiLegacyHandlers() __attribute__((constructor));
void setupAmiLegacyHandlers() {
  LOG_INFO("registering AMI legacy + MDR V2 handlers on NetFn 0x3A/0x32");
  ami::seedFromBakedIfMissing();
  loadWorkingBuffer();

  for (ipmi::NetFn nf : {ipmi::netFnOemSix}) { // ipmi::netFnOemTwo
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
