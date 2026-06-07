// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock-specific OEM IPMI commands for the X570D4I-2T BMC.
//
// Currently implements:
//   cmdGetBoardInfo (0x50) – return product ID and board revision from
//   the D-Bus Inventory so that host utilities can identify the BMC.

#include <oemcommands.hpp>

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <string>

namespace asrock
{

// -----------------------------------------------------------------------
// D-Bus paths for board inventory
// -----------------------------------------------------------------------

static constexpr const char* boardObjPath =
    "/xyz/openbmc_project/inventory/system/board";
static constexpr const char* itemBoardIntf =
    "xyz.openbmc_project.Inventory.Item.Board";
static constexpr const char* assetTagIntf =
    "xyz.openbmc_project.Inventory.Decorator.Asset";

// -----------------------------------------------------------------------
// Forward declaration for the constructor
// -----------------------------------------------------------------------

static void registerOEMFunctions() __attribute__((constructor));

// -----------------------------------------------------------------------
// Board info command
// -----------------------------------------------------------------------

/**
 * @brief GetBoardInfo (0x50) – return board identification.
 *
 * Response layout:
 *   Byte 0    : completion code
 *   Bytes 1-N : null-terminated product-name string (up to 64 bytes)
 *
 * If the D-Bus inventory is not yet available the response carries a
 * hard-coded fallback identifier so the command never fails.
 */
ipmi::RspType<std::vector<uint8_t>>
    ipmiGetBoardInfo(ipmi::Context::ptr& ctx)
{
    std::string productName = "ASRock X570D4I-2T"; // fallback

    try
    {
        auto dbus = getSdBus();

        std::string service =
            ipmi::getService(*dbus, itemBoardIntf, boardObjPath);

        ipmi::Value nameVariant = ipmi::getDbusProperty(
            *dbus, service, boardObjPath, assetTagIntf, "Model");

        const auto& name = std::get<std::string>(nameVariant);
        if (!name.empty())
        {
            productName = name;
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ipmiGetBoardInfo: D-Bus lookup failed, using fallback",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }

    // Pack as a null-terminated byte vector (max 64 bytes incl. null)
    if (productName.size() > 63)
    {
        productName.resize(63);
    }
    productName.push_back('\0');

    std::vector<uint8_t> data(productName.begin(), productName.end());
    return ipmi::responseSuccess(data);
}

// -----------------------------------------------------------------------
// Handler registration
// -----------------------------------------------------------------------

static void registerOEMFunctions()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock OEM commands registered");

    ipmi::registerHandler(ipmi::prioOemBase,
                          static_cast<ipmi::NetFn>(netFnGeneral),
                          static_cast<ipmi::Cmd>(general::cmdGetBoardInfo),
                          ipmi::Privilege::User, ipmiGetBoardInfo);
}

} // namespace asrock
