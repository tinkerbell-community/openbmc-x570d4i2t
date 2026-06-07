// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// BIOS OOB (Out-of-Band) configuration data structures for the
// ASRock X570D4I-2T.  The protocol mirrors the one implemented in
// intel-ipmi-oem/src/biosconfigcommands.cpp so that any UEFI firmware
// implementing the standard OOB payload-transfer commands works
// without modifications.

#pragma once

#include <cstdint>

namespace asrock
{

// Maximum number of payload slots supported
static constexpr uint8_t maxPayloadSupported = 3;
// Maximum block returned by GetPayload/GetPayloadData in one shot
static constexpr uint16_t maxGetPayloadDataSize = 4096;

// BIOS OOB capability bit: bit 1 set means OOB BIOS config is supported
static constexpr uint8_t biosCapOobSupported = (1 << 1);

// -----------------------------------------------------------------------
// Payload type identifiers
// -----------------------------------------------------------------------
enum class PType : uint8_t
{
    // Payload 0: lzcat-compressed BIOS setup XML
    BIOSXMLType0 = 0,
    // Payload 1: pending BIOS attributes (key=value, plain text)
    BIOSXMLType1 = 1,
    // Payload 5: OTA firmware image
    OTAPayload   = 5,
};

// -----------------------------------------------------------------------
// Multi-step transfer state machine
// -----------------------------------------------------------------------
enum class PTState : uint8_t
{
    StartTransfer = 0,
    InProgress    = 1,
    EndTransfer   = 2,
    UserAbort     = 3,
};

// Payload validity status
enum class PStatus : uint8_t
{
    Unknown   = 0,
    Valid     = 1,
    Corrupted = 2,
};

// GetPayload selector values
enum class GetPayloadParameter : uint8_t
{
    GetPayloadInfo   = 0,
    GetPayloadData   = 1,
    GetPayloadStatus = 2,
    MaxPayloadParameters,
};

// -----------------------------------------------------------------------
// Wire-format structures (all little-endian, packed)
// -----------------------------------------------------------------------

// StartTransfer body (sent by BIOS to open a new transfer session)
struct __attribute__((packed)) PayloadStartTransfer
{
    uint32_t payloadTotalChecksum; // CRC-32 of the complete payload
    uint32_t payloadTotalSize;     // total byte count
    uint32_t payloadVersion;       // protocol version (pass-through)
    uint32_t payloadflag;          // payload flags (pass-through)
};

// InProgress body (sent once per chunk)
struct __attribute__((packed)) PayloadInProgress
{
    uint32_t payloadReservationID;   // must match value returned by StartTransfer
    uint32_t payloadCurrentSize;     // byte count of the chunk in this message
    uint32_t payloadOffset;          // byte offset within the complete payload
    uint32_t payloadCurrentChecksum; // CRC-32 of the chunk data
    // chunk data follows immediately
};

// EndTransfer / UserAbort body
struct __attribute__((packed)) PayloadEndTransfer
{
    uint32_t payloadReservationID; // must match StartTransfer reservation
};

// -----------------------------------------------------------------------
// In-memory metadata for one payload slot
// -----------------------------------------------------------------------
struct PayloadInfo
{
    uint32_t payloadReservationID      = 0;
    uint32_t payloadTotalChecksum      = 0;
    uint32_t payloadCurrentChecksum    = 0;
    uint32_t payloadTimeStamp          = 0;
    uint32_t payloadTotalSize          = 0;
    uint32_t payloadCurrentSize        = 0;
    uint32_t actualTotalPayloadWritten = 0;
    uint8_t  payloadVersion            = 0;
    uint8_t  payloadType               = 0;
    uint8_t  payloadStatus             = 0;
    uint8_t  payloadflag               = 0;
};

// BIOS OOB capability flags
struct BIOSCapabilities
{
    uint8_t OOBCapability = 0;
};

// -----------------------------------------------------------------------
// NV-backed structure written to /var/oob/nvoobdata.dat
// -----------------------------------------------------------------------
struct NVOOBdata
{
    BIOSCapabilities mBIOSCapabilities = {};
    bool             mIsBIOSCapInitDone = false;
    PayloadInfo      payloadInfo[maxPayloadSupported] = {};
};

} // namespace asrock
