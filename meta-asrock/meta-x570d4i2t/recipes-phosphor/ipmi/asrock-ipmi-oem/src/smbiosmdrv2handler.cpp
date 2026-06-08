// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// AMI Aptio V SMBIOS push handlers for the X570D4I-2T BMC.
//
// The ASRock X570D4I-2T BIOS uses AMI-proprietary IPMI commands on NetFn 0x3A
// (netFnOemSix) to push SMBIOS data to the BMC, NOT the standard Intel MDR2
// protocol on NetFn 0x3E.
//
// Two surfaces are implemented:
//
//   AMI proprietary push (from BIOS modules SendInfoBmcIpmiDxe, ChassisIdDxe,
//   BackupBmcMacDxe, SetBiosInfoDxe):
//     0xF3  GetStatus      — BIOS polls BMC readiness
//     0xB2  SetBiosInfo    — BIOS overlays 16 bytes into SMBIOS Type 0
//     0xA0  SetMdrPos      — actually BackupBmcMacDxe in this BIOS RE;
//                            body = [LAN_channel:1][mac:6]
//     0xA1  GetMdrStatus   — BIOS reads current region size + checksum
//     0xB5  SetSmbiosChunk — body=[reserved:1=0x00][ASCII...][NUL:1];
//                            each call auto-advances through kAmiSlotTable
//
//   AMI MDR V2 (reader/writer over IPMI):
//     0x30  AgentStatus   — BIOS probes MDR agent readiness
//     0x31  GetDir        — BIOS reads region directory
//     0x3D  GetStatus     — BIOS reads MDR region status
//     0x51  WriteBegin    — BIOS opens a write session
//     0x52  WriteChunk    — BIOS sends SMBIOS chunks
//     0x53  WriteEnd      — BIOS signals done; BMC persists + triggers sync
//     0x71  RegionStatus  — BIOS reads per-region status
//     0x72  GetBlock      — BIOS/BMC reads SMBIOS or meta-header region
//
// All handlers are registered at prioOpenBmcBase on netFnOemSix (0x3A) so
// they take precedence over any lower-priority handlers.
//
// At library load, g_amiBuf is seeded from /var/lib/smbios/smbios2 then
// overlaid with FRU EEPROM data (/sys/bus/i2c/devices/7-0057/eeprom).
//
// Reference: tinkerbell-community/openbmc-x570d4i2t branch oem-ipmi,
//            ami-ipmi-oem/legacy.cpp

#include <amiconverter.hpp>
#include <oemcommands.hpp>

#include <boost/asio/post.hpp>
#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace asrock {

// ── Command codes on NetFn 0x3A ──────────────────────────────────────────────

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
constexpr ipmi::Cmd kCmdAmiSetMdrPos = 0xA0;
constexpr ipmi::Cmd kCmdAmiGetMdrStatus = 0xA1;
constexpr ipmi::Cmd kCmdAmiSetChunk = 0xB5;

constexpr uint8_t kRegionSmbios = 0;
constexpr uint8_t kRegionMeta = 1;

// ── MDR V2 write-machine state
// ────────────────────────────────────────────────

enum class MdrWriteState : uint8_t { Idle, Open, Receiving };
static MdrWriteState g_mdrState = MdrWriteState::Idle;
static uint32_t g_mdrDeclaredSize = 0;
static std::vector<uint8_t> g_mdrBuf;

// ── 0xB5 SetSmbiosChunk slot table ───────────────────────────────────────────
//
// The BIOS sends strings in order, cycling through this table. Each 0xB5 call
// auto-advances g_amiCallIndex so the next call targets the next slot.

struct StringSlot {
  uint8_t smbiosType;
  uint8_t stringIndex; // 1-based within the structure's string area
  const char *label;
};
constexpr StringSlot kAmiSlotTable[] = {
    {1, 1, "Type1.SystemManufacturer"},
    {1, 2, "Type1.SystemProductName"},
    {2, 1, "Type2.BoardManufacturer"},
    {2, 2, "Type2.BoardProductName"},
};
constexpr size_t kNumAmiSlots =
    sizeof(kAmiSlotTable) / sizeof(kAmiSlotTable[0]);
static size_t g_amiCallIndex = 0;

// In-memory working copy of the SMBIOS payload (no MDR or AMI header).
// Seeded at load time from smbios2 then overlaid with FRU EEPROM data.
static std::vector<uint8_t> g_amiBuf;

// ── Forward declarations
// ──────────────────────────────────────────────────────

static void registerAmiSmbiosHandlers() __attribute__((constructor));

// ── SMBIOS structure navigation
// ───────────────────────────────────────────────

// Return byte offset of the first structure of `type` in `buf`.
// Skips the _SM3_ (24-byte) or _SM_ (31-byte) anchor at the head.
// Returns SIZE_MAX if not found.
static size_t findSmbiosStructOffset(const std::vector<uint8_t> &buf,
                                     uint8_t type) {
  size_t pos = 0;
  if (buf.size() >= 5 && buf[0] == '_' && buf[1] == 'S' && buf[2] == 'M' &&
      buf[3] == '3' && buf[4] == '_') {
    pos = 24;
  } else if (buf.size() >= 4 && buf[0] == '_' && buf[1] == 'S' &&
             buf[2] == 'M' && buf[3] == '_') {
    pos = 31;
  }
  while (pos + 4 <= buf.size()) {
    uint8_t structType = buf[pos];
    uint8_t structLen = buf[pos + 1];
    if (structLen < 4)
      break;
    if (structType == type)
      return pos;
    if (structType == 127)
      break; // end-of-table marker
    size_t end = pos + structLen;
    while (end + 1 < buf.size() && !(buf[end] == 0 && buf[end + 1] == 0)) {
      end++;
    }
    end += 2;
    if (end > buf.size())
      break;
    pos = end;
  }
  return SIZE_MAX;
}

// Replace the `stringIndex`-th string (1-based) in the structure of `type`.
// Resizes `buf` in place when the replacement differs in length.
// Returns true on success.
static bool replaceSmbiosString(std::vector<uint8_t> &buf, uint8_t type,
                                uint8_t stringIndex,
                                const std::string &newStr) {
  if (stringIndex == 0)
    return false;
  size_t structOff = findSmbiosStructOffset(buf, type);
  if (structOff == SIZE_MAX) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "replaceSmbiosString: struct type not found",
        phosphor::logging::entry("TYPE=%u", type));
    return false;
  }
  uint8_t structLen = buf[structOff + 1];
  size_t stringAreaStart = structOff + structLen;
  if (stringAreaStart > buf.size())
    return false;

  size_t pos = stringAreaStart;
  size_t curIdx = 1;
  size_t targetStart = SIZE_MAX;
  size_t targetEnd = SIZE_MAX;

  while (pos < buf.size()) {
    if (buf[pos] == 0) {
      if (pos + 1 < buf.size() && buf[pos + 1] == 0 && pos == stringAreaStart) {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "replaceSmbiosString: empty string area",
            phosphor::logging::entry("TYPE=%u", type));
        return false;
      }
      if (curIdx == stringIndex) {
        targetEnd = pos;
        break;
      }
      ++curIdx;
      ++pos;
      if (pos < buf.size() && buf[pos] == 0) {
        targetStart = pos;
        targetEnd = pos;
        break;
      }
      if (curIdx == stringIndex)
        targetStart = pos;
      continue;
    }
    if (curIdx == stringIndex && targetStart == SIZE_MAX)
      targetStart = pos;
    ++pos;
  }
  if (curIdx == stringIndex && targetStart != SIZE_MAX &&
      targetEnd == SIZE_MAX) {
    targetEnd = pos;
  }
  if (targetStart == SIZE_MAX || targetEnd == SIZE_MAX)
    return false;

  size_t oldLen = targetEnd - targetStart;
  size_t newLen = newStr.size();
  if (newLen == oldLen) {
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
  } else if (newLen < oldLen) {
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
    buf.erase(buf.begin() + targetStart + newLen, buf.begin() + targetEnd);
  } else {
    size_t growBy = newLen - oldLen;
    buf.insert(buf.begin() + targetEnd, growBy, 0);
    std::memcpy(buf.data() + targetStart, newStr.data(), newLen);
  }
  return true;
}

// Read the string-index byte from `fieldOffset` within the structure's
// formatted area; returns 0 if the structure or offset is not found.
static uint8_t smbiosFieldStrIdx(const std::vector<uint8_t> &buf, uint8_t type,
                                 size_t fieldOffset) {
  size_t off = findSmbiosStructOffset(buf, type);
  if (off == SIZE_MAX || off + fieldOffset >= buf.size())
    return 0;
  return buf[off + fieldOffset];
}

// ── Working buffer management
// ─────────────────────────────────────────────────

static void loadWorkingBuffer() {
  g_amiBuf = ami::loadMdrPayload();
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "loadWorkingBuffer",
      phosphor::logging::entry("BYTES=%zu", g_amiBuf.size()),
      phosphor::logging::entry("PATH=%s", ami::kSmbiosFile));
}

static void persistWorkingBuffer() {
  if (g_amiBuf.empty()) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "persistWorkingBuffer: empty buffer — skipping");
    return;
  }
  if (!ami::writeMdrFile(g_amiBuf.data(), g_amiBuf.size())) {
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "persistWorkingBuffer: writeMdrFile failed");
    return;
  }
  if (!ami::triggerMdrSync()) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "persistWorkingBuffer: file written but MDR sync failed");
  }
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "persistWorkingBuffer: synced",
      phosphor::logging::entry("BYTES=%zu", g_amiBuf.size()));
}

static void commitMdrBuffer() {
  if (!g_mdrBuf.empty()) {
    if (ami::persistAmiBuffer(g_mdrBuf.data(), g_mdrBuf.size())) {
      ami::triggerMdrSync();
      phosphor::logging::log<phosphor::logging::level::INFO>(
          "MDR V2 write committed",
          phosphor::logging::entry("BYTES=%zu", g_mdrBuf.size()));
    } else {
      phosphor::logging::log<phosphor::logging::level::ERR>(
          "MDR V2 write commit failed");
    }
    g_mdrBuf.clear();
  }
  g_mdrState = MdrWriteState::Idle;
  g_mdrDeclaredSize = 0;
}

// ── FRU EEPROM → SMBIOS overlay ──────────────────────────────────────────────

static constexpr const char *kFruEepromPath =
    "/sys/bus/i2c/devices/7-0057/eeprom";

// Advance past one IPMI FRU type/length-prefixed field. Returns the ASCII
// string value (type 3 fields only); sets done=true on 0xC1 or overrun.
static std::string fruNextField(const std::vector<uint8_t> &data, size_t &pos,
                                bool &done) {
  if (pos >= data.size()) {
    done = true;
    return {};
  }
  uint8_t tl = data[pos++];
  if (tl == 0xC1) {
    done = true;
    return {};
  }
  uint8_t type = (tl >> 6) & 0x03;
  uint8_t len = tl & 0x3F;
  if (pos + len > data.size()) {
    done = true;
    return {};
  }
  std::string s;
  if (type == 3 && len > 0)
    s.assign(reinterpret_cast<const char *>(data.data() + pos), len);
  pos += len;
  return s;
}

static void populateSmbiosFromFru() {
  std::ifstream f(kFruEepromPath, std::ios::binary);
  if (!f) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "populateSmbiosFromFru: cannot open FRU EEPROM",
        phosphor::logging::entry("PATH=%s", kFruEepromPath));
    return;
  }
  std::vector<uint8_t> fru(std::istreambuf_iterator<char>(f), {});
  if (fru.size() < 8 || fru[0] != 0x01) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "populateSmbiosFromFru: invalid FRU header");
    return;
  }

  // Board Area: version(1) len(1) lang(1) mfgdate(3), then fields
  std::string boardMfr, boardProduct, boardSerial;
  size_t boardOff = static_cast<size_t>(fru[3]) * 8;
  if (boardOff + 6 < fru.size() && fru[boardOff] == 0x01) {
    size_t pos = boardOff + 6;
    bool done = false;
    boardMfr = fruNextField(fru, pos, done);
    boardProduct = fruNextField(fru, pos, done);
    boardSerial = fruNextField(fru, pos, done);
  }

  // Product Area: version(1) len(1) lang(1), then fields; scan for UUID
  std::string productSerial;
  std::array<uint8_t, 16> uuid{};
  bool haveUuid = false;
  size_t productOff = static_cast<size_t>(fru[4]) * 8;
  if (productOff + 3 < fru.size() && fru[productOff] == 0x01) {
    size_t areaLen = static_cast<size_t>(fru[productOff + 1]) * 8;
    size_t areaEnd = std::min(productOff + areaLen, fru.size());
    size_t pos = productOff + 3;
    bool done = false;
    fruNextField(fru, pos, done); // manufacturer
    fruNextField(fru, pos, done); // product name
    fruNextField(fru, pos, done); // part/model
    fruNextField(fru, pos, done); // version
    productSerial = fruNextField(fru, pos, done);
    fruNextField(fru, pos, done); // asset tag
    fruNextField(fru, pos, done); // fru file id
    while (!done && pos < areaEnd) {
      std::string extra = fruNextField(fru, pos, done);
      if (extra.size() == 32) {
        bool ok = true;
        for (char c : extra)
          if (!std::isxdigit(static_cast<unsigned char>(c)))
            ok = false;
        if (ok) {
          for (int i = 0; i < 16; ++i) {
            char h[3] = {extra[i * 2], extra[i * 2 + 1], '\0'};
            uuid[i] = static_cast<uint8_t>(std::strtoul(h, nullptr, 16));
          }
          haveUuid = true;
        }
      }
    }
  }

  phosphor::logging::log<phosphor::logging::level::INFO>(
      "populateSmbiosFromFru",
      phosphor::logging::entry("BOARD_MFR=%s", boardMfr.c_str()),
      phosphor::logging::entry("BOARD_PRODUCT=%s", boardProduct.c_str()),
      phosphor::logging::entry("BOARD_SERIAL=%s", boardSerial.c_str()));

  if (g_amiBuf.empty()) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "populateSmbiosFromFru: g_amiBuf empty — skipping overlay");
    return;
  }

  bool changed = false;
  const std::string &serial =
      productSerial.empty() ? boardSerial : productSerial;

  // Type 2 (Baseboard): Manufacturer @ +4, Product @ +5, Serial @ +7
  if (!boardMfr.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 2, 4);
    if (idx && replaceSmbiosString(g_amiBuf, 2, idx, boardMfr))
      changed = true;
  }
  if (!boardProduct.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 2, 5);
    if (idx && replaceSmbiosString(g_amiBuf, 2, idx, boardProduct))
      changed = true;
  }
  if (!boardSerial.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 2, 7);
    if (idx && replaceSmbiosString(g_amiBuf, 2, idx, boardSerial))
      changed = true;
  }

  // Type 1 (System): Manufacturer @ +4, Product @ +5, Serial @ +7
  if (!boardMfr.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 1, 4);
    if (idx && replaceSmbiosString(g_amiBuf, 1, idx, boardMfr))
      changed = true;
  }
  if (!boardProduct.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 1, 5);
    if (idx && replaceSmbiosString(g_amiBuf, 1, idx, boardProduct))
      changed = true;
  }
  if (!serial.empty()) {
    uint8_t idx = smbiosFieldStrIdx(g_amiBuf, 1, 7);
    if (idx && replaceSmbiosString(g_amiBuf, 1, idx, serial))
      changed = true;
  }

  // Type 1 UUID: 16 bytes at structure offset +8
  if (haveUuid) {
    size_t off = findSmbiosStructOffset(g_amiBuf, 1);
    uint8_t slen = (off != SIZE_MAX) ? g_amiBuf[off + 1] : 0;
    if (off != SIZE_MAX && slen >= 24 && off + 24 <= g_amiBuf.size()) {
      std::memcpy(g_amiBuf.data() + off + 8, uuid.data(), 16);
      changed = true;
    }
  }

  // Type 3 (System Enclosure / Chassis): Rack Mount + ASRockRack
  {
    size_t t3off = findSmbiosStructOffset(g_amiBuf, 3);
    if (t3off != SIZE_MAX && t3off + 5 < g_amiBuf.size()) {
      if (g_amiBuf[t3off + 5] != 0x11) {
        g_amiBuf[t3off + 5] = 0x11; // Rack Mount chassis type
        changed = true;
      }
    }
    uint8_t t3mfr = smbiosFieldStrIdx(g_amiBuf, 3, 4);
    if (t3mfr && replaceSmbiosString(g_amiBuf, 3, t3mfr, "ASRockRack"))
      changed = true;
    if (!boardSerial.empty()) {
      uint8_t t3ser = smbiosFieldStrIdx(g_amiBuf, 3, 7);
      if (t3ser && replaceSmbiosString(g_amiBuf, 3, t3ser, boardSerial))
        changed = true;
    }
  }

  // Type 4 (Processor): AMD family byte 0x18, fix manufacturer + version
  {
    size_t t4off = findSmbiosStructOffset(g_amiBuf, 4);
    if (t4off != SIZE_MAX && t4off + 6 < g_amiBuf.size()) {
      if (g_amiBuf[t4off + 6] != 0x18) {
        g_amiBuf[t4off + 6] = 0x18; // AMD processor family
        changed = true;
      }
    }
    uint8_t t4mfr = smbiosFieldStrIdx(g_amiBuf, 4, 7);
    if (t4mfr &&
        replaceSmbiosString(g_amiBuf, 4, t4mfr, "Advanced Micro Devices, Inc."))
      changed = true;
    uint8_t t4ver = smbiosFieldStrIdx(g_amiBuf, 4, 0x10);
    if (t4ver && replaceSmbiosString(g_amiBuf, 4, t4ver, "AMD Processor"))
      changed = true;
  }

  if (changed) {
    persistWorkingBuffer();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "populateSmbiosFromFru: SMBIOS updated from FRU EEPROM");
  } else {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "populateSmbiosFromFru: no changes — strings may already match");
  }
}

// ── AMI MDR V2 handlers
// ───────────────────────────────────────────────────────

static ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>
hMdrAgentStatus(ipmi::Context::ptr) {
  uint8_t dataInit = g_amiBuf.empty() ? 0x01 : 0x00;
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0x30 MdrAgentStatus",
      phosphor::logging::entry("BUF_BYTES=%zu", g_amiBuf.size()),
      phosphor::logging::entry("DATA_INIT=%u", dataInit));
  return ipmi::responseSuccess(uint8_t{0x01}, uint8_t{0x01}, uint8_t{0x00},
                               uint8_t{0x01}, dataInit);
}

static ipmi::RspType<std::vector<uint8_t>>
hMdrGetDir(ipmi::Context::ptr, std::vector<uint8_t> req) {
  uint16_t agentId  = (req.size() >= 2)
                          ? static_cast<uint16_t>(req[0] | (req[1] << 8))
                          : 0;
  uint8_t  dirIndex = (req.size() >= 3) ? req[2] : 0;

  // Always report SMBIOS region as valid=0 so the BIOS pushes its tables.
  // Our FRU-seeded g_amiBuf is a fallback; the BIOS copy is authoritative.
  constexpr uint16_t kMaxSmbios = 0xFFFF;

  // Response: [version:1, entryCount:1, remaining:1, <entries>]
  // Each 16-byte entry: [regionId, valid, usedSz:2LE, allocSz:2LE, maxSz:2LE, chk, pad*7]
  std::vector<uint8_t> rsp(3 + 16, 0);
  rsp[0] = 0x01; // version
  rsp[1] = 0x01; // 1 entry returned (SMBIOS only)
  rsp[2] = 0x00; // 0 remaining

  uint8_t* e = rsp.data() + 3;
  e[0] = kRegionSmbios;
  e[1] = 0x00;                            // valid=0: BMC ready to receive
  e[2] = 0x00; e[3] = 0x00;              // usedSize=0
  e[4] = kMaxSmbios & 0xFF;              // allocSize lo
  e[5] = (kMaxSmbios >> 8) & 0xFF;       // allocSize hi
  e[6] = kMaxSmbios & 0xFF;              // maxSize lo
  e[7] = (kMaxSmbios >> 8) & 0xFF;       // maxSize hi
  e[8] = 0x00;                            // checksum=0 (no data)

  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0x31 MdrGetDir",
      phosphor::logging::entry("AGENT=0x%04X", agentId),
      phosphor::logging::entry("DIR_INDEX=%u", dirIndex));
  return ipmi::responseSuccess(rsp);
}

static ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t>
hMdrGetStatus(ipmi::Context::ptr) {
  phosphor::logging::log<phosphor::logging::level::DEBUG>(
      "AMI 0x3D MdrGetStatus");
  return ipmi::responseSuccess(uint8_t{0x00}, uint8_t{0x00}, uint8_t{0x10},
                               uint8_t{0x01});
}

static ipmi::RspType<> hMdrWriteBegin(ipmi::Context::ptr, uint8_t regionId,
                                      uint16_t declaredSize) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0x51 MdrWriteBegin", phosphor::logging::entry("REGION=%u", regionId),
      phosphor::logging::entry("DECLARED=%u", declaredSize));
  g_mdrBuf.clear();
  g_mdrBuf.reserve(declaredSize);
  g_mdrDeclaredSize = declaredSize;
  g_mdrState = MdrWriteState::Open;
  return ipmi::responseSuccess();
}

static ipmi::RspType<> hMdrWriteChunk(ipmi::Context::ptr, uint8_t regionId,
                                      uint16_t offset,
                                      std::vector<uint8_t> payload) {
  size_t needed = static_cast<size_t>(offset) + payload.size();
  if (needed > ami::kMaxPayload + sizeof(ami::AmiMdrHeader)) {
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "AMI 0x52 MdrWriteChunk: oversized",
        phosphor::logging::entry("NEEDED=%zu", needed));
    return ipmi::responseReqDataLenInvalid();
  }
  if (needed > g_mdrBuf.size())
    g_mdrBuf.resize(needed, 0);
  std::copy(payload.begin(), payload.end(), g_mdrBuf.begin() + offset);
  g_mdrState = MdrWriteState::Receiving;
  phosphor::logging::log<phosphor::logging::level::DEBUG>(
      "AMI 0x52 MdrWriteChunk", phosphor::logging::entry("REGION=%u", regionId),
      phosphor::logging::entry("OFFSET=%u", offset),
      phosphor::logging::entry("LEN=%zu", payload.size()));
  return ipmi::responseSuccess();
}

static ipmi::RspType<> hMdrWriteEnd(ipmi::Context::ptr) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0x53 MdrWriteEnd",
      phosphor::logging::entry("BUF_BYTES=%zu", g_mdrBuf.size()),
      phosphor::logging::entry("DECLARED=%u", g_mdrDeclaredSize));
  commitMdrBuffer();
  return ipmi::responseSuccess();
}

static ipmi::RspType<std::vector<uint8_t>> hMdrRegionStatus(ipmi::Context::ptr,
                                                            uint8_t regionId) {
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

  phosphor::logging::log<phosphor::logging::level::DEBUG>(
      "AMI 0x71 MdrRegionStatus",
      phosphor::logging::entry("REGION=%u", regionId),
      phosphor::logging::entry("VALID=%u", valid));
  return ipmi::responseSuccess(rsp);
}

static ipmi::RspType<std::vector<uint8_t>>
hMdrGetBlock(ipmi::Context::ptr, uint8_t regionId, uint16_t offset) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0x72 MdrGetBlock", phosphor::logging::entry("REGION=%u", regionId),
      phosphor::logging::entry("OFFSET=%u", offset));

  if (regionId == kRegionSmbios) {
    if (offset >= g_amiBuf.size())
      return ipmi::responseSuccess(std::vector<uint8_t>{});
    return ipmi::responseSuccess(
        std::vector<uint8_t>(g_amiBuf.begin() + offset, g_amiBuf.end()));
  }
  if (regionId == kRegionMeta) {
    ami::AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(g_amiBuf.size());
    hdr.checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    uint8_t hdrBytes[sizeof(hdr)];
    std::memcpy(hdrBytes, &hdr, sizeof(hdr));
    if (offset >= sizeof(hdrBytes))
      return ipmi::responseSuccess(std::vector<uint8_t>{});
    return ipmi::responseSuccess(
        std::vector<uint8_t>(hdrBytes + offset, hdrBytes + sizeof(hdrBytes)));
  }
  return ipmi::responseSuccess(std::vector<uint8_t>{});
}

// ── AMI proprietary handlers
// ───────────────────────────────────────────────────

static ipmi::RspType<> hAmiGetStatus(ipmi::Context::ptr) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0xF3 GetStatus",
      phosphor::logging::entry("BUF_BYTES=%zu", g_amiBuf.size()));
  return ipmi::responseSuccess();
}

// 0xA1 GetMdrStatus — BIOS reads current size + checksum of a region.
// Response: [regionId:1, valid:1, dataSize:2 LE, checksum:2 LE]
static ipmi::RspType<std::vector<uint8_t>>
hAmiGetMdrStatus(ipmi::Context::ptr, std::vector<uint8_t> req) {
  uint8_t regionId = req.empty() ? 0 : req[0];
  uint16_t dataSize = 0;
  uint16_t checksum = 0;
  uint8_t valid = 0;

  if (regionId == kRegionSmbios) {
    dataSize = static_cast<uint16_t>(g_amiBuf.size());
    checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    valid = g_amiBuf.empty() ? 0 : 1;
  } else if (regionId == kRegionMeta) {
    ami::AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(g_amiBuf.size());
    hdr.checksum = ami::computeChecksum(g_amiBuf.data(), g_amiBuf.size());
    dataSize = sizeof(hdr);
    uint8_t hdrBytes[sizeof(hdr)];
    std::memcpy(hdrBytes, &hdr, sizeof(hdr));
    checksum = ami::computeChecksum(hdrBytes, sizeof(hdrBytes));
    valid = 1;
  }

  std::vector<uint8_t> rsp{
      regionId,
      valid,
      static_cast<uint8_t>(dataSize & 0xFF),
      static_cast<uint8_t>((dataSize >> 8) & 0xFF),
      static_cast<uint8_t>(checksum & 0xFF),
      static_cast<uint8_t>((checksum >> 8) & 0xFF),
  };
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0xA1 GetMdrStatus", phosphor::logging::entry("REGION=%u", regionId),
      phosphor::logging::entry("VALID=%u", valid),
      phosphor::logging::entry("DATA_SIZE=%u", dataSize));
  return ipmi::responseSuccess(rsp);
}

// 0xB2 SetBiosInfo — BIOS overlays its info into SMBIOS Type 0 at offset +4.
static ipmi::RspType<> hAmiSetBiosInfo(ipmi::Context::ptr,
                                       std::vector<uint8_t> payload) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0xB2 SetBiosInfo",
      phosphor::logging::entry("BYTES=%zu", payload.size()));

  if (payload.empty())
    return ipmi::responseSuccess();

  size_t type0Off = findSmbiosStructOffset(g_amiBuf, 0);
  if (type0Off == SIZE_MAX) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "hAmiSetBiosInfo: Type 0 not found in working buffer");
    return ipmi::responseSuccess();
  }

  size_t fieldsOff = type0Off + 4;
  uint8_t structLen = g_amiBuf[type0Off + 1];
  size_t maxOverlay = (structLen > 4) ? (structLen - 4) : 0;
  size_t overlayLen = std::min(payload.size(), maxOverlay);

  if (overlayLen == 0 || fieldsOff + overlayLen > g_amiBuf.size())
    return ipmi::responseSuccess();

  std::copy(payload.begin(), payload.begin() + overlayLen,
            g_amiBuf.begin() + fieldsOff);
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "hAmiSetBiosInfo: overlaid Type 0",
      phosphor::logging::entry("BYTES=%zu", overlayLen));
  persistWorkingBuffer();
  return ipmi::responseSuccess();
}

// 0xA0 SetMdrPos — actually BackupBmcMacDxe in this BIOS RE.
// Body = [LAN_channel:1][mac:6]. We accept and log it.
static ipmi::RspType<> hAmiSetMdrPos(ipmi::Context::ptr, uint8_t region,
                                     uint16_t offset) {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0xA0 SetMdrPos [BIOS: BackupBmcMacDxe LAN_channel + MAC bytes]",
      phosphor::logging::entry("REGION=%u", region),
      phosphor::logging::entry("OFFSET=0x%04X", offset));
  return ipmi::responseSuccess();
}

// 0xB5 SetSmbiosChunk — wire format (from BIOS RE of SendInfoBmcIpmiDxe):
//   [reserved:1=0x00][ASCII string...][NUL:1]
// Each call auto-advances g_amiCallIndex to the next entry in kAmiSlotTable.
// Returns 1 byte ack (0x01 = OK) to satisfy the BIOS transport's ResponseSize
// check; returns 0x00 (without advancing) if string replacement fails.
static ipmi::RspType<uint8_t> hAmiSetChunk(ipmi::Context::ptr,
                                           std::vector<uint8_t> raw) {
  if (raw.empty()) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "AMI 0xB5 SetSmbiosChunk: empty body");
    return ipmi::responseSuccess(uint8_t{0x00});
  }
  if (raw[0] != 0x00) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "AMI 0xB5 SetSmbiosChunk: reserved byte non-zero",
        phosphor::logging::entry("BYTE=0x%02X", raw[0]));
  }

  // Strip leading reserved byte and trailing NUL
  size_t startIdx = 1;
  size_t endIdx = raw.size();
  if (endIdx > startIdx && raw[endIdx - 1] == 0x00)
    endIdx--;
  std::string str(raw.begin() + startIdx, raw.begin() + endIdx);

  const StringSlot &target = kAmiSlotTable[g_amiCallIndex % kNumAmiSlots];
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI 0xB5 SetSmbiosChunk",
      phosphor::logging::entry("CALL_IDX=%zu", g_amiCallIndex),
      phosphor::logging::entry("SLOT=%s", target.label),
      phosphor::logging::entry("VALUE=%s", str.c_str()));

  bool changed =
      replaceSmbiosString(g_amiBuf, target.smbiosType, target.stringIndex, str);
  if (!changed) {
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "AMI 0xB5: string replacement failed — slot index NOT advanced",
        phosphor::logging::entry("SLOT=%s", target.label));
    return ipmi::responseSuccess(uint8_t{0x00});
  }

  persistWorkingBuffer();
  ++g_amiCallIndex;
  return ipmi::responseSuccess(uint8_t{0x01});
}

// ── Registration
// ──────────────────────────────────────────────────────────────

static void registerAmiSmbiosHandlers() {
  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI SMBIOS: seeding working buffer");
  ami::seedFromBakedIfMissing();
  loadWorkingBuffer();
  // populateSmbiosFromFru() is deferred — see boost::asio::post below.
  // Running it here blocked the event loop for 2+ seconds (synchronous I2C
  // EEPROM read), causing KCS commands queued by kcsbridged during POST to
  // time out before our handlers were visible.

  phosphor::logging::log<phosphor::logging::level::INFO>(
      "AMI SMBIOS: registering 13 handlers on NetFn 0x3A (prioOpenBmcBase)");

  for (ipmi::NetFn nf : {ipmi::netFnOemSix, ipmi::netFnOemTwo, static_cast<ipmi::NetFn>(0x04)}) {

    // Register only on netFnOemSix (0x3A): the ASRock BIOS uses this NetFn for
    // all AMI MDR and proprietary SMBIOS commands.  Do NOT add netFnOemTwo
    // (0x32) or the Sensor NetFn (0x04) here — those collide with legitimate
    // sensor and storage commands at overlapping command codes.
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrAgentStatus, ipmi::Privilege::Admin,
                          hMdrAgentStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf, kCmdMdrGetDir,
                          ipmi::Privilege::Admin, hMdrGetDir);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrGetStatus, ipmi::Privilege::Admin,
                          hMdrGetStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrWriteBegin, ipmi::Privilege::Admin,
                          hMdrWriteBegin);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrWriteChunk, ipmi::Privilege::Admin,
                          hMdrWriteChunk);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrWriteEnd, ipmi::Privilege::Admin, hMdrWriteEnd);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrRegionStatus, ipmi::Privilege::Admin,
                          hMdrRegionStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdMdrGetBlock, ipmi::Privilege::Admin, hMdrGetBlock);

    // AMI proprietary
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdAmiGetStatus, ipmi::Privilege::Admin,
                          hAmiGetStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdAmiSetBiosInfo, ipmi::Privilege::Admin,
                          hAmiSetBiosInfo);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdAmiGetMdrStatus, ipmi::Privilege::Admin,
                          hAmiGetMdrStatus);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdAmiSetMdrPos, ipmi::Privilege::Admin,
                          hAmiSetMdrPos);
    ipmi::registerHandler(ipmi::prioOpenBmcBase, nf,
                          kCmdAmiSetChunk, ipmi::Privilege::Admin, hAmiSetChunk);

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "AMI SMBIOS: handlers ready");

  }

  // Defer the FRU EEPROM read to the first event-loop tick so it does not
  // block handler registration or in-flight KCS commands during startup.
  boost::asio::post(*getIoContext(), []() { populateSmbiosFromFru(); });
}

} // namespace asrock
