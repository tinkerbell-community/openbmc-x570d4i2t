// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI command codes for the X570D4I-2T.
//
// NetFn assignments follow the original AMI Megarac SPX firmware extracted
// from bmc-analyze (libipmimsghndlr.so g_AMI_CmdHndlr table) and the
// megarac-bios-ipmi-methods implementation reference.
//
// NetFn 0x30 (NETFN_AMI / OEM General):
//   - BIOS OOB configuration protocol
//   - AMI YAFU-replacement stubs
//   - Board-info and sensor-info commands
//   - KVM mux, PECI, and PSU OEM handlers
//
// NetFn 0x3E (NETFN_TEST_OEM / OEM Eight):
//   - MDR2 SMBIOS transfer protocol
//
// Command codes are sourced from:
//   - Confirmed g_AMI_CmdHndlr table extraction (bmc-analyze.instructions.md)
//   - Megarac implementation reference (megarac-bios-ipmi-methods.instructions.md)
//   - intel-ipmi-oem smbiosmdrv2handler.cpp (MDR2 codes, identical protocol)

#pragma once

#include <cstdint>

namespace asrock
{

// -----------------------------------------------------------------------
// Network Functions
// -----------------------------------------------------------------------

// NetFn 0x30 — AMI OEM General (NETFN_AMI in Megarac)
// Used for all AMI/ASRock OEM commands: BIOS OOB, sensor queries,
// KVM mux, PECI, PSU, firmware version, and board identity.
static constexpr uint8_t netFnGeneral = 0x30;

// NetFn 0x3E — OEM Eight (NETFN_TEST_OEM in Megarac)
// Used for MDR2 SMBIOS transfer commands, matching the convention from
// intel-ipmi-oem so that BIOS firmware built against the phosphor MDR2
// protocol works without modification.
static constexpr uint8_t netFnMdr = 0x3E;

// -----------------------------------------------------------------------
// NETFN_AMI (0x30) command codes
// -----------------------------------------------------------------------

namespace general
{

// ------------------------------------------------------------------
// BIOS OOB configuration commands
// Confirmed: megarac-bios-ipmi-methods §2.2
// ------------------------------------------------------------------

// Capability negotiation – BIOS declares OOB support during POST
static constexpr uint8_t cmdSetBIOSCap  = 0x7F; // Admin
static constexpr uint8_t cmdGetBIOSCap  = 0x7E; // User

// Chunked payload transfer (BIOS→BMC for config XML / BMC→BIOS for pending)
static constexpr uint8_t cmdSetPayload  = 0x73; // Admin
static constexpr uint8_t cmdGetPayload  = 0x72; // User

// ------------------------------------------------------------------
// AMI YAFU (firmware upload) — codes 0x01–0x10
// Confirmed: g_AMI_CmdHndlr (bmc-analyze §MDR)
// OpenBMC equivalent: phosphor-ipmi-blobs
// These stubs exist so the host does not receive "invalid command"
// when the BIOS probes for YAFU support; they return
// ipmi::responseInvalidCommand() to signal the feature is absent.
// ------------------------------------------------------------------

static constexpr uint8_t cmdYafuAllocateMemory   = 0x01; // Admin
static constexpr uint8_t cmdYafuFreeMemory        = 0x02; // Admin
static constexpr uint8_t cmdYafuReadMemory        = 0x03; // Admin
static constexpr uint8_t cmdYafuWriteMemory       = 0x04; // Admin
static constexpr uint8_t cmdYafuCopyMemory        = 0x05; // Admin
static constexpr uint8_t cmdYafuCompareMemory     = 0x06; // Admin
static constexpr uint8_t cmdYafuClearMemory       = 0x07; // Admin
static constexpr uint8_t cmdYafuReadFlash         = 0x08; // Admin
static constexpr uint8_t cmdYafuWriteFlash        = 0x09; // Admin
static constexpr uint8_t cmdYafuEraseFlash        = 0x0A; // Admin
static constexpr uint8_t cmdYafuVerifyFlash       = 0x0B; // Admin
static constexpr uint8_t cmdYafuActivateFlash     = 0x0C; // Admin
static constexpr uint8_t cmdYafuGetStatus         = 0x0D; // Admin
static constexpr uint8_t cmdYafuGetFirmwareInfo   = 0x0E; // Admin
static constexpr uint8_t cmdYafuGetFlashInfo      = 0x0F; // Admin
static constexpr uint8_t cmdYafuEraseCopyFlash    = 0x10; // disabled (0xFF priv)

// ------------------------------------------------------------------
// Sensor / inventory info
// Confirmed: g_AMI_CmdHndlr table (bmc-analyze §NETFN_AMI)
// ------------------------------------------------------------------

// Read sensor metadata from D-Bus dbus-sensors (USER)
static constexpr uint8_t cmdGetSensorInfo  = 0x1E; // User

// ------------------------------------------------------------------
// Firmware version / protocol info
// Confirmed: g_AMI_CmdHndlr table (bmc-analyze §NETFN_AMI)
// ------------------------------------------------------------------

static constexpr uint8_t cmdGetFwVersion     = 0x20; // priv 0x10 (User)
static constexpr uint8_t cmdGetFwProtocol    = 0x21; // priv 0x10 (User)

// ------------------------------------------------------------------
// BMC configuration management
// Confirmed: g_AMI_CmdHndlr (bmc-analyze §NETFN_AMI)
// ------------------------------------------------------------------

static constexpr uint8_t cmdManageBmcConfig  = 0x2B; // priv 0x01 (User)

// ------------------------------------------------------------------
// SEL policy commands
// Confirmed: g_AMI_CmdHndlr (bmc-analyze §NETFN_AMI)
// ------------------------------------------------------------------

static constexpr uint8_t cmdGetSelPolicy  = 0x30; // priv 0x13 (Admin)
static constexpr uint8_t cmdSetSelPolicy  = 0x31; // priv 0xFF (disabled)

// ------------------------------------------------------------------
// ASRock board-specific
// Confirmed: megarac-bios-ipmi-methods §2.2
// ------------------------------------------------------------------

// Return board product name from D-Bus FRU inventory
static constexpr uint8_t cmdGetBoardInfo  = 0x50; // User

// ------------------------------------------------------------------
// KVM mux switching — controls GPIOJ1 (line 73) to mux SPI flash
// Confirmed: g_AMI_CmdHndlr 0xEE CMD_AMI_MUX_SWITCHING
// Critical OEM command: bmc-analyze §Critical OEM Commands
// ------------------------------------------------------------------

static constexpr uint8_t cmdMuxSwitching  = 0xEE; // priv 0x01 (User)

// ------------------------------------------------------------------
// PECI read/write — CPU thermal via AMD APML/PECI
// Confirmed: g_AMI_CmdHndlr 0xE9 CMD_AMI_PECI_READ_WRITE
// Critical OEM command: bmc-analyze §Critical OEM Commands
// ------------------------------------------------------------------

static constexpr uint8_t cmdPeciReadWrite  = 0xE9; // priv 0x03 (User)

// ------------------------------------------------------------------
// PSU information aggregation
// Confirmed: g_AMI_CmdHndlr 0xEC CMD_AMI_PSU_INFO
// Critical OEM command: bmc-analyze §Critical OEM Commands
// ------------------------------------------------------------------

static constexpr uint8_t cmdPsuInfo  = 0xEC; // priv 0xFF (User-visible)

} // namespace general

// -----------------------------------------------------------------------
// NETFN_TEST_OEM (0x3E) — MDR2 SMBIOS transfer commands
// Command codes mirror intel-ipmi-oem convention exactly so that any
// BIOS firmware implementing the phosphor OOB MDR2 protocol works
// without modification.
// Reference: megarac-bios-ipmi-methods §3.2
// -----------------------------------------------------------------------

namespace mdr
{

static constexpr uint8_t cmdMdrIIAgentStatus      = 0x30;
static constexpr uint8_t cmdMdrIIGetDir           = 0x31;
static constexpr uint8_t cmdMdrIIGetDataInfo      = 0x32;
static constexpr uint8_t cmdMdrIILockData         = 0x33;
static constexpr uint8_t cmdMdrIIUnlockData       = 0x34;
static constexpr uint8_t cmdMdrIIGetDataBlock     = 0x35;

static constexpr uint8_t cmdMdrIISendDir          = 0x38;
static constexpr uint8_t cmdMdrIISendDataInfoOffer = 0x39;
static constexpr uint8_t cmdMdrIISendDataInfo     = 0x3A;
static constexpr uint8_t cmdMdrIIDataStart        = 0x3B;
static constexpr uint8_t cmdMdrIIDataDone         = 0x3C;
static constexpr uint8_t cmdMdrIISendDataBlock    = 0x3D;

} // namespace mdr

// -----------------------------------------------------------------------
// IPMI completion codes specific to ASRock OEM commands
// Reference: megarac-bios-ipmi-methods §2.2
// -----------------------------------------------------------------------

namespace cc
{

static constexpr uint8_t payloadPacketMissed  = 0x80; // lzcat decompression failed
static constexpr uint8_t payloadChecksumFail  = 0x81; // CRC-32 mismatch in InProgress
static constexpr uint8_t notSupportedInState  = 0x82; // host OS at Standby (POST done)
static constexpr uint8_t payloadIncomplete    = 0x83; // EndTransfer before all chunks received
static constexpr uint8_t biosCapNotInit       = 0x85; // SetBIOSCap not yet called

} // namespace cc

} // namespace asrock
