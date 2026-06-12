// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// SMBIOS table synthesizer for the X570D4I-2T.
//
// The AMI host BIOS pushes only two OEM fields over IPMI AMI-MDR (BIOS version
// via 0xB2 SetBiosInfo, board/product name via 0xB5 SetSmbiosChunk) and then
// READS the full SMBIOS table back via 0x72 GetBlock. So the BMC must hold a
// complete table. This module synthesizes one from:
//   - captured host fields (Type 0 BIOS version; Type 1/2 product name)
//   - FruDevice D-Bus inventory  (Type 1/2 manufacturer / serial / part)
//   - DDR4 SPD via spd::readAllDimms (Type 16/17 size/speed/mfr/part/serial)
//   - static board constants      (Type 3 chassis, Type 4 processor socket)
//
// The emitted payload begins with the SMBIOS structure table directly (Type 0
// at offset 0 — NO _SM3_ entry point and NO leading prefix): smbios-mdr's
// getSMBIOSTypePtr() walks structures from offset 0 of the region that
// readDataFromFlash() hands it (file data just past the 10-byte MDRSMBIOSHeader).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smbiosbuild
{

// Capture host-pushed fields (called from the 0xB2 / 0xB5 IPMI handlers).
void setHostBios(const std::string& version, const std::string& releaseDate);
void setHostBoardName(const std::string& productName);

// Build the full SMBIOS structure table (Type 0 first, Type 127 last).
// Returns the raw payload to hand to ami::writeMdrFile (which prepends the
// 10-byte MDRSMBIOSHeader). Caches the result: the expensive SPD/FRU reads run
// only when a host field has changed (see needsRebuild); otherwise the cached
// table is returned. This keeps the BIOS's repeated 0x5D commits cheap and the
// served table byte-stable so the host's GetBlock read-back converges.
std::vector<uint8_t> buildSmbiosTable();

// True if a fresh build is needed (host field changed, or never built). When
// false, the persisted table is already current and the commit can fast-path.
bool needsRebuild();

} // namespace smbiosbuild
