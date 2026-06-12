// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// AMI management command set (NETFN_AMI = 0x32) name lookup. See amicommands.cpp.

#pragma once

#include <cstdint>

namespace asrock
{

// Human-readable name for an AMI (NetFn 0x32) command code, mirrored from the
// stock sync-agent ipmi_commands.lua AMI_CMD table, or "AMI_UNKNOWN".
const char* amiCommandName(uint8_t cmd);

} // namespace asrock
