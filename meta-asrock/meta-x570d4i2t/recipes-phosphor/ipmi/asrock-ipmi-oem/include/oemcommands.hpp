// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI command codes for the X570D4I-2T.
//
// NetFn assignments follow the original AMI Megarac SPX firmware extracted
// from bmc-analyze (libipmimsghndlr.so g_AMI_CmdHndlr table) and the
// megarac-bios-ipmi-methods implementation reference.
//
// NetFn 0x3A (ipmi::netFnOemSix / NETFN_AMI):
//   - AMI YAFU-replacement stubs
//   - Board-info and sensor-info commands
//   - KVM mux, PECI, and PSU OEM handlers
//
// SMBIOS IS carried over IPMI on this board: the AMI host BIOS pushes SMBIOS
// fragments via the AMI-MDR command set (NetFn 0x3A 0xB5 SetSmbiosChunk / 0xB2,
// NetFn 0x32 0x5D LegacyCtrl), handled here; the BMC synthesizes the table from
// FRU/SPD. BIOS configuration is served by stock bmcweb /Bios routes backed by
// BIOSConfigManager — there is NO host-push path (the USB Redfish Host
// Interface approach and its bmcweb OEM patches were removed entirely).
//
// Command codes are sourced from:
//   - Confirmed g_AMI_CmdHndlr table extraction (bmc-analyze.instructions.md)
//   - Megarac implementation reference (megarac-bios-ipmi-methods.instructions.md)

#pragma once

#include <cstdint>

namespace asrock
{

// -----------------------------------------------------------------------
// Network Functions
// -----------------------------------------------------------------------

// All AMI/ASRock OEM commands use NetFn 0x3A (ipmi::netFnOemSix):
// sensor queries, KVM mux, PECI, PSU, firmware version, board ID.

// -----------------------------------------------------------------------
// netFnOemSix (0x3A) command codes
// -----------------------------------------------------------------------

namespace general
{

// NOTE: BIOS configuration has no host-push path on this board.  The Intel-style
// IPMI OOB payload commands (SetBIOSCap/GetBIOSCap/SetPayload/GetPayload) that
// once lived here were removed: this firmware never issues them (verified by
// boot-time KCS capture).  The AMI in-band Redfish Host Interface push path was
// also removed entirely (USB gadget + bmcweb OEM routes).  The stock bmcweb
// /Bios routes remain, backed by xyz.openbmc_project.BIOSConfigManager (empty).

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
// AMI inventory query
// Confirmed: g_AMI_CmdHndlr 0xE6 CMD_AMI_GET_INVENTORY
// Note: 0x50 = CMD_AMI_GET_SERVICE_CONF (different command entirely)
// ------------------------------------------------------------------

// Return board/system inventory data from D-Bus FRU + Software objects
static constexpr uint8_t cmdGetInventory  = 0xE6; // User

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

// NOTE: SMBIOS IS received over IPMI on this board, via the AMI-MDR command
// set — NetFn 0x3A 0xB5 SetSmbiosChunk / 0xB2, NetFn 0x32 0x5D LegacyCtrl —
// handled in src/oemcommands.cpp (see amiconverter.{cpp,hpp}).  The BMC
// accumulates the host-pushed fragments and synthesizes the SMBIOS table from
// FRU/SPD, writes /var/lib/smbios/smbios2, then calls smbios-mdrv2
// AgentSynchronizeData.  The standard IPMI MDR2 (NetFn 0x3E) path is NOT used —
// this firmware never issues it (verified by live busctl capture).

} // namespace asrock
