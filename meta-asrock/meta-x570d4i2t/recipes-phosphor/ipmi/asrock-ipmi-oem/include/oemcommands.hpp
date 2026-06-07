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

// -----------------------------------------------------------------------
// Network Functions
// -----------------------------------------------------------------------

// NetFn 0x30 — OEM/site-specific (ASRock OEM General)
// Used for BIOS OOB config and board-specific commands.
static constexpr uint8_t netFnGeneral = 0x30;

// NetFn 0x3E — OEM Eight (ipmi::netFnOemEight)
// Used for MDR2 SMBIOS transfer commands, matching the convention
// established by intel-ipmi-oem/src/smbiosmdrv2handler.cpp so that
// any BIOS firmware built against the phosphor OOB MDR2 protocol
// works without modification.
static constexpr uint8_t netFnMdr = 0x3E;

namespace general
{
// ------------------------------------------------------------------
// BIOS OOB configuration commands  (NetFn 0x30)
// ------------------------------------------------------------------

// Capability negotiation – BIOS calls this first to declare OOB support
static constexpr uint8_t cmdSetBIOSCap = 0x7F;
static constexpr uint8_t cmdGetBIOSCap = 0x7E;

// Chunked payload transfer (BIOS→BMC for config XML, BMC→BIOS for pending)
static constexpr uint8_t cmdSetPayload = 0x73;
static constexpr uint8_t cmdGetPayload = 0x72;

// ------------------------------------------------------------------
// ASRock board-specific OEM commands  (NetFn 0x30)
// ------------------------------------------------------------------

// Return the board product ID / revision read from the FRU EEPROM
static constexpr uint8_t cmdGetBoardInfo = 0x50;

} // namespace general

namespace mdr
{
// ------------------------------------------------------------------
// MDR2 SMBIOS transfer commands  (NetFn 0x3E)
// Command codes mirror ipmi::intel::app in intel-ipmi-oem.
// ------------------------------------------------------------------

// Agent status / directory negotiation
static constexpr uint8_t cmdMdrIIAgentStatus     = 0x30;
static constexpr uint8_t cmdMdrIIGetDir          = 0x31;
static constexpr uint8_t cmdMdrIIGetDataInfo     = 0x32; // ← GetDataInfo
static constexpr uint8_t cmdMdrIILockData        = 0x33;
static constexpr uint8_t cmdMdrIIUnlockData      = 0x34;
static constexpr uint8_t cmdMdrIIGetDataBlock    = 0x35;

// BIOS → BMC push sequence
static constexpr uint8_t cmdMdrIISendDir         = 0x38;
static constexpr uint8_t cmdMdrIISendDataInfoOffer = 0x39;
static constexpr uint8_t cmdMdrIISendDataInfo    = 0x3A;
static constexpr uint8_t cmdMdrIIDataStart       = 0x3B;
static constexpr uint8_t cmdMdrIIDataDone        = 0x3C;
static constexpr uint8_t cmdMdrIISendDataBlock   = 0x3D;

} // namespace mdr

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
