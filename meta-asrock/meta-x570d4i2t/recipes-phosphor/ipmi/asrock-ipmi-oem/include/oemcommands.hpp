// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI command codes.
//
// NetFn 0x30 (OEM/General) is used throughout for parity with the
// intel-ipmi-oem ecosystem so that host firmware implementing the
// standard phosphor OOB BIOS-config protocol works without changes.

#pragma once

#include <cstdint>

namespace asrock
{

// ASRock OEM General NetFn (0x30)
static constexpr uint8_t netFnGeneral = 0x30;

namespace general
{
// ------------------------------------------------------------------
// BIOS OOB configuration commands
// (same command codes as intel-ipmi-oem for cross-vendor compat)
// ------------------------------------------------------------------

// Capability negotiation – BIOS calls this first to declare OOB support
static constexpr uint8_t cmdSetBIOSCap = 0x7F;
static constexpr uint8_t cmdGetBIOSCap = 0x7E;

// Chunked payload transfer (BIOS→BMC for config XML, BMC→BIOS for pending)
static constexpr uint8_t cmdSetPayload = 0x73;
static constexpr uint8_t cmdGetPayload = 0x72;

// ------------------------------------------------------------------
// ASRock board-specific OEM commands
// ------------------------------------------------------------------

// Return the board product ID / revision read from the FRU EEPROM
static constexpr uint8_t cmdGetBoardInfo = 0x50;

} // namespace general

// ------------------------------------------------------------------
// IPMI completion codes specific to ASRock OEM commands
// ------------------------------------------------------------------
namespace cc
{
static constexpr uint8_t payloadPacketMissed  = 0x80;
static constexpr uint8_t payloadChecksumFail  = 0x81;
static constexpr uint8_t notSupportedInState  = 0x82;
static constexpr uint8_t payloadIncomplete    = 0x83;
static constexpr uint8_t biosCapNotInit       = 0x85;
} // namespace cc

} // namespace asrock
