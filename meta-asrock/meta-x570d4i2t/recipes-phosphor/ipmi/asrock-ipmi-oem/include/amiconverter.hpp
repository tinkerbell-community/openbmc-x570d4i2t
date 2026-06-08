// SPDX-License-Identifier: Apache-2.0
//
// Format conversion between the AMI Aptio V SMBIOS view used over the wire
// (NetFn 0x3A AMI/MDR endpoints) and the OpenBMC MDR V2 on-disk file consumed
// by smbios-mdrv2.
//
// AMI Aptio V view (over the wire):
//     [ AmiMdrHeader { u16 dataSize, u16 checksum } ][ payload ... ]
//   where payload is the anchor (_SM3_ / _SM_) + structure table, dataSize is
//   the byte length of payload, and checksum is sum-of-bytes(payload) & 0xFFFF.
//
// MDR V2 on-disk view (/var/lib/smbios/smbios2):
//     [ MDRSMBIOSHeader { u8 dirVer=2, u8 mdrType=2, u32 timestamp,
//                         u32 dataSize } ][ payload ... ]
//   where payload is the same anchor + structure table.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ami
{

inline constexpr const char* kSmbiosDir  = "/var/lib/smbios";
inline constexpr const char* kSmbiosFile = "/var/lib/smbios/smbios2";
inline constexpr const char* kBakedDmp   = "/usr/share/x570d4i2t/smbios.dmp";

inline constexpr uint8_t  kMdrDirVersion = 0x02;
inline constexpr uint8_t  kMdrTypeII     = 0x02;
inline constexpr uint32_t kMaxPayload    = 64 * 1024;

struct MDRSMBIOSHeader
{
    uint8_t  dirVersion;
    uint8_t  mdrType;
    uint32_t timestamp;
    uint32_t dataSize;
} __attribute__((packed));
static_assert(sizeof(MDRSMBIOSHeader) == 10, "MDRSMBIOSHeader size mismatch");

struct AmiMdrHeader
{
    uint16_t dataSize;
    uint16_t checksum;
} __attribute__((packed));
static_assert(sizeof(AmiMdrHeader) == 4, "AmiMdrHeader size mismatch");

uint16_t computeChecksum(const uint8_t* data, size_t len);

// Read /var/lib/smbios/smbios2 and return just the payload (no MDR header).
std::vector<uint8_t> loadMdrPayload();

// Return AMI Aptio V view of the current cache: [AmiMdrHeader][payload].
std::vector<uint8_t> amiViewFromCache();

// Persist payload as MDR V2 on disk (prepends MDRSMBIOSHeader).
bool writeMdrFile(const uint8_t* payload, size_t len);

// Parse an AMI Aptio V buffer (with leading AmiMdrHeader) and persist the
// payload as MDR V2. If the leading header parses cleanly (dataSize + remainder
// equals total length, and checksum matches), the prefix is stripped; otherwise
// the whole buffer is treated as raw payload.
bool persistAmiBuffer(const uint8_t* amiBytes, size_t len);

// Trigger smbios-mdrv2 AgentSynchronizeData via D-Bus.
bool triggerMdrSync();

// One-shot seed of the cache from the baked dump if no file exists yet.
void seedFromBakedIfMissing();

} // namespace ami
