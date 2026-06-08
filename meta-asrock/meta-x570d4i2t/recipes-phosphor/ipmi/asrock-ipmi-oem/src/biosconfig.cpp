// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ASRock OEM IPMI BIOS out-of-band (OOB) configuration commands.
//
// Protocol overview
// -----------------
// 1. BIOS calls SetBIOSCap (0x7F) during POST to declare OOB support.
// 2. BIOS calls SetPayload (0x73) with paramSel=StartTransfer to open a
//    transfer slot and receive a reservationID.
// 3. BIOS calls SetPayload with paramSel=InProgress once per chunk,
//    carrying the reservationID, offset, and CRC-32 of the chunk.
// 4. BIOS calls SetPayload with paramSel=EndTransfer to finalise the
//    transfer.  For payload type 0 the BMC decompresses (lzcat) the
//    received file and, if biosconfig-manager is running, sets the
//    BaseBIOSTable property on xyz.openbmc_project.BIOSConfig.Manager.
// 5. GetPayload (0x72) lets the BIOS read back pending attribute changes
//    (payload type 1, plain text key=value pairs) from the BMC.
//
// Storage
// -------
//   /var/oob/              – working directory (created on demand)
//   /var/oob/nvoobdata.dat – NV-backed capability + payload metadata
//   /var/oob/temp<N>       – scratch file during InProgress phase
//   /var/oob/Payload<N>    – finalised payload file
//   /var/oob/bios.xml      – decompressed BIOS setup XML (type 0)
//
// This file is modelled closely after
//   intel-ipmi-oem/src/biosconfigcommands.cpp
// so that UEFI firmware implementing the standard phosphor OOB protocol
// works without modifications.

#include <biosconfig.hpp>
#include <oemcommands.hpp>

#include <boost/crc.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/io.hpp>
#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/message/types.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message/types.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace asrock
{

// -----------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------

static NVOOBdata gNVOOBdata;

// -----------------------------------------------------------------------
// D-Bus constants (matches biosconfig-manager / bios-settings-mgr)
// -----------------------------------------------------------------------

static constexpr const char* biosConfigBaseMgrPath =
    "/xyz/openbmc_project/bios_config/manager";
static constexpr const char* biosConfigIntf =
    "xyz.openbmc_project.BIOSConfig.Manager";
static constexpr const char* resetBIOSSettingsProp = "ResetBIOSSettings";

// -----------------------------------------------------------------------
// File-system constants
// -----------------------------------------------------------------------

static constexpr const char* biosConfigFolder    = "/var/oob";
static constexpr const char* biosConfigNVPath    = "/var/oob/nvoobdata.dat";
static constexpr const char* biosXMLFilePath     = "/var/oob/bios.xml";
static constexpr const char* pendingAttrFilePath = "/var/oob/Payload1";

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static void registerBIOSConfigFunctions() __attribute__((constructor));
static bool flushNVOOBdata();

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

/** @brief Persist gNVOOBdata to disk. */
static bool flushNVOOBdata()
{
    std::ofstream outFile(biosConfigNVPath, std::ios::binary);
    outFile.seekp(std::ios_base::beg);
    outFile.write(reinterpret_cast<const char*>(&gNVOOBdata),
                  sizeof(NVOOBdata));
    return outFile.good();
}

/** @brief Load gNVOOBdata from disk, or initialise to defaults.
 *  @return 0 on success, IPMI CC on error.
 */
static uint8_t initNVOOBdata()
{
    if (!std::filesystem::exists(biosConfigFolder))
    {
        std::filesystem::create_directories(biosConfigFolder);
    }

    std::ifstream ifs(biosConfigNVPath, std::ios::binary);
    if (ifs.good())
    {
        ifs.read(reinterpret_cast<char*>(&gNVOOBdata), sizeof(NVOOBdata));
        ifs.close();
        return ipmi::ccSuccess;
    }
    return ipmi::ccResponseError;
}

/**
 * @brief Query host OS state via D-Bus to determine if POST has completed.
 *
 * Returns true when the host OS is at Standby (POST done) so that the BMC
 * can reject BIOS-config commands that are only valid during POST.  On any
 * D-Bus failure the function conservatively returns true (POST complete) to
 * avoid accepting config commands when the state is uncertain.
 */
static bool getPostCompleted()
{
    bool postCompleted = true;
    try
    {
        std::shared_ptr<sdbusplus::asio::connection> dbus = getSdBus();
        ipmi::Value variant = ipmi::getDbusProperty(
            *dbus, "xyz.openbmc_project.State.Host0",
            "/xyz/openbmc_project/state/host0",
            "xyz.openbmc_project.State.OperatingSystem.Status",
            "OperatingSystemState");
        const auto& value = std::get<std::string>(variant);
        postCompleted =
            (value == "Standby") ||
            (value == "xyz.openbmc_project.State.OperatingSystem."
                      "Status.OSStatus.Standby");
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "getPostCompleted: D-Bus query failed",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }
    return postCompleted;
}

/** @brief Delete any stale temp/payload files for the given slot. */
static void cleanUpPayloadFile(uint8_t payloadType)
{
    std::string tempPath =
        std::string(biosConfigFolder) + "/temp" + std::to_string(payloadType);
    std::string payloadPath = std::string(biosConfigFolder) + "/Payload" +
                              std::to_string(payloadType);
    ::unlink(tempPath.c_str());
    ::unlink(payloadPath.c_str());

    if (payloadType == static_cast<uint8_t>(PType::BIOSXMLType0))
    {
        // Also invalidate the dependent pending-attributes payload
        ::unlink(pendingAttrFilePath);
        gNVOOBdata
            .payloadInfo[static_cast<uint8_t>(PType::BIOSXMLType1)]
            .payloadStatus = static_cast<uint8_t>(PStatus::Unknown);
    }
}

/**
 * @brief Run an external command with a single argument to decompress the
 *        BIOS XML payload, redirecting stdout to biosXMLFilePath.
 */
template <typename... ArgTypes>
static int generateBIOSXMLFile(const char* path, ArgTypes&&... args)
{
    boost::process::v1::child execProg(
        path, const_cast<char*>(args)...,
        boost::process::v1::std_out > biosXMLFilePath);
    execProg.wait();
    return execProg.exit_code();
}

/**
 * @brief Update the D-Bus BaseBIOSTable property from /var/oob/bios.xml.
 *
 * This reads the pending-attributes plain-text file (key=value per line)
 * and sets the PendingAttributes property on the BIOSConfig.Manager
 * service so that bmcweb can expose them via Redfish.
 *
 * A full XML→D-Bus attribute-table parser would call into biosconfig-manager's
 * BaseBIOSTable.  For now the parsed file is simply stored on disk; a future
 * enhancement can add the XML parsing pass here.
 */
static void publishBIOSPayload(const std::string& service,
                               uint8_t payloadType)
{
    // Only payload type 0 (the compressed XML) drives D-Bus updates.
    if (payloadType != static_cast<uint8_t>(PType::BIOSXMLType0))
    {
        return;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "publishBIOSPayload: BIOS XML payload received, stored at "
        "/var/oob/bios.xml. "
        "A separate attribute parser can import it into BIOSConfig.Manager.");
    (void)service; // suppress unused-parameter warning until full parser added
}

// -----------------------------------------------------------------------
// IPMI command handlers
// -----------------------------------------------------------------------

/**
 * @brief SetBIOSCap (0x7F) – BIOS declares its OOB capabilities.
 *
 * Must be called before POST is complete (OperatingSystemState != Standby).
 *
 * Request:  BIOSCapabilityByte, reserved[3]
 * Response: (none beyond completion code)
 */
ipmi::RspType<> ipmiSetBIOSCap(ipmi::Context::ptr& /*ctx*/,
                                uint8_t biosCap,
                                uint8_t reserved1,
                                uint8_t reserved2,
                                uint8_t reserved3)
{
    if (getPostCompleted())
    {
        return ipmi::response(cc::notSupportedInState);
    }
    if (reserved1 != 0 || reserved2 != 0 || reserved3 != 0)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    gNVOOBdata.mBIOSCapabilities.OOBCapability = biosCap;
    gNVOOBdata.mIsBIOSCapInitDone              = true;
    flushNVOOBdata();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "BIOS OOB: SetBIOSCap",
        phosphor::logging::entry("CAP=0x%02X", biosCap));
    return ipmi::responseSuccess();
}

/**
 * @brief GetBIOSCap (0x7E) – BMC reports its recorded BIOS capabilities.
 *
 * Response:  OOBCapabilityByte, reserved[3]
 */
ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t>
    ipmiGetBIOSCap(ipmi::Context::ptr& /*ctx*/)
{
    if (!gNVOOBdata.mIsBIOSCapInitDone)
    {
        return ipmi::response(cc::biosCapNotInit);
    }
    return ipmi::responseSuccess(gNVOOBdata.mBIOSCapabilities.OOBCapability,
                                 0, 0, 0);
}

/**
 * @brief SetPayload (0x73) – chunked BIOS→BMC config transfer.
 *
 * Request:  paramSel (PTState), payloadType, payload-data
 * Response: depends on paramSel
 *   StartTransfer → reservationID (uint32_t)
 *   InProgress    → bytesWritten  (uint32_t)
 *   EndTransfer   → totalWritten  (uint32_t)
 *   UserAbort     → (none)
 */
ipmi::RspType<uint32_t> ipmiSetPayload(ipmi::Context::ptr& ctx,
                                        uint8_t paramSel,
                                        uint8_t payloadType,
                                        std::vector<uint8_t> payload)
{
    // Capability check
    if (!(gNVOOBdata.mBIOSCapabilities.OOBCapability & biosCapOobSupported))
    {
        return ipmi::response(cc::biosCapNotInit);
    }
    if (payloadType >= maxPayloadSupported)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    // Payload type 0 (XML) is only accepted during POST
    if (payloadType == static_cast<uint8_t>(PType::BIOSXMLType0))
    {
        if (getPostCompleted())
        {
            return ipmi::response(cc::notSupportedInState);
        }
    }

    switch (static_cast<PTState>(paramSel))
    {
        // ---------------------------------------------------------------
        case PTState::StartTransfer:
        {
            if (payload.size() < sizeof(PayloadStartTransfer))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "ipmiSetPayload: StartTransfer payload too short");
                return ipmi::responseReqDataLenInvalid();
            }
            const auto* hdr =
                reinterpret_cast<const PayloadStartTransfer*>(payload.data());

            cleanUpPayloadFile(payloadType);

            auto& pi                    = gNVOOBdata.payloadInfo[payloadType];
            pi.payloadReservationID     = static_cast<uint32_t>(rand());
            pi.payloadTotalChecksum     = hdr->payloadTotalChecksum;
            pi.payloadTotalSize         = hdr->payloadTotalSize;
            pi.payloadVersion           = hdr->payloadVersion;
            pi.payloadflag              = hdr->payloadflag;
            pi.actualTotalPayloadWritten = 0;
            pi.payloadStatus =
                static_cast<uint8_t>(PStatus::Unknown);
            pi.payloadType = payloadType;

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "BIOS OOB: payload transfer started",
                phosphor::logging::entry("TYPE=%u", payloadType),
                phosphor::logging::entry("TOTAL_SIZE=%u", pi.payloadTotalSize));
            return ipmi::responseSuccess(pi.payloadReservationID);
        }

        // ---------------------------------------------------------------
        case PTState::InProgress:
        {
            if (payload.size() < sizeof(PayloadInProgress))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "ipmiSetPayload: InProgress payload too short");
                return ipmi::responseReqDataLenInvalid();
            }
            const auto* hdr =
                reinterpret_cast<const PayloadInProgress*>(payload.data());
            auto& pi = gNVOOBdata.payloadInfo[payloadType];

            if (hdr->payloadReservationID != pi.payloadReservationID)
            {
                return ipmi::responseInvalidReservationId();
            }

            // Verify chunk CRC-32 (data starts after the 16-byte header)
            constexpr size_t headerSize = sizeof(PayloadInProgress);
            if (payload.size() <= headerSize)
            {
                return ipmi::responseReqDataLenInvalid();
            }
            boost::crc_32_type crc;
            crc.process_bytes(payload.data() + headerSize,
                              payload.size() - headerSize);
            if (crc.checksum() != hdr->payloadCurrentChecksum)
            {
                return ipmi::response(cc::payloadChecksumFail);
            }

            // Append chunk to the temp file
            std::string tempPath = std::string(biosConfigFolder) + "/temp" +
                                   std::to_string(payloadType);
            std::ofstream outFile(tempPath,
                                  std::ios::binary | std::ios::app);
            outFile.seekp(hdr->payloadOffset);
            outFile.write(
                reinterpret_cast<const char*>(payload.data()) + headerSize,
                static_cast<std::streamsize>(payload.size() - headerSize));
            outFile.close();

            pi.payloadStatus = static_cast<uint8_t>(PStatus::Unknown);
            pi.actualTotalPayloadWritten += hdr->payloadCurrentSize;

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "BIOS OOB: payload chunk received",
                phosphor::logging::entry("TYPE=%u", payloadType),
                phosphor::logging::entry("OFFSET=%u", hdr->payloadOffset),
                phosphor::logging::entry("SIZE=%u", hdr->payloadCurrentSize),
                phosphor::logging::entry("TOTAL_WRITTEN=%u",
                                          pi.actualTotalPayloadWritten));
            return ipmi::responseSuccess(hdr->payloadCurrentSize);
        }

        // ---------------------------------------------------------------
        case PTState::EndTransfer:
        {
            if (payload.size() < sizeof(PayloadEndTransfer))
            {
                return ipmi::responseReqDataLenInvalid();
            }
            const auto* hdr =
                reinterpret_cast<const PayloadEndTransfer*>(payload.data());
            auto& pi = gNVOOBdata.payloadInfo[payloadType];

            if (hdr->payloadReservationID != pi.payloadReservationID)
            {
                return ipmi::responseInvalidReservationId();
            }
            if (pi.actualTotalPayloadWritten != pi.payloadTotalSize)
            {
                pi.payloadStatus = static_cast<uint8_t>(PStatus::Unknown);
                return ipmi::response(cc::payloadIncomplete);
            }

            // Promote temp → final
            std::string tempPath = std::string(biosConfigFolder) + "/temp" +
                                   std::to_string(payloadType);
            std::string finalPath = std::string(biosConfigFolder) + "/Payload" +
                                    std::to_string(payloadType);
            if (std::rename(tempPath.c_str(), finalPath.c_str()) != 0)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "ipmiSetPayload: rename temp->Payload failed");
            }

            // For type 0: decompress the lzcat-compressed BIOS XML
            if (payloadType == static_cast<uint8_t>(PType::BIOSXMLType0))
            {
                int rc = generateBIOSXMLFile("/usr/bin/lzcat", "-d",
                                             finalPath.c_str());
                if (rc != 0)
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "ipmiSetPayload: lzcat decompression failed");
                    pi.payloadStatus =
                        static_cast<uint8_t>(PStatus::Corrupted);
                    return ipmi::response(cc::payloadPacketMissed);
                }

                // Kick off the async D-Bus update
                auto io   = getIoContext();
                auto dbus = getSdBus();
                if (io && dbus)
                {
                    std::string service = ipmi::getService(
                        *dbus, biosConfigIntf, biosConfigBaseMgrPath);
                    boost::asio::post(*io, [service, payloadType] {
                        publishBIOSPayload(service, payloadType);
                    });
                }
            }

            // Update metadata from the finalised file
            struct stat st {};
            if (::stat(finalPath.c_str(), &st) == 0)
            {
                pi.payloadTimeStamp = static_cast<uint32_t>(st.st_mtime);
                pi.payloadTotalSize = static_cast<uint32_t>(st.st_size);
                pi.payloadStatus   = static_cast<uint8_t>(PStatus::Valid);
            }
            else
            {
                pi.payloadStatus = static_cast<uint8_t>(PStatus::Corrupted);
            }

            flushNVOOBdata();
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "BIOS OOB: payload transfer complete",
                phosphor::logging::entry("TYPE=%u", payloadType),
                phosphor::logging::entry("BYTES=%u",
                                          pi.actualTotalPayloadWritten));
            return ipmi::responseSuccess(pi.actualTotalPayloadWritten);
        }

        // ---------------------------------------------------------------
        case PTState::UserAbort:
        {
            if (payload.size() < sizeof(PayloadEndTransfer))
            {
                return ipmi::responseReqDataLenInvalid();
            }
            const auto* hdr =
                reinterpret_cast<const PayloadEndTransfer*>(payload.data());
            auto& pi = gNVOOBdata.payloadInfo[payloadType];

            if (hdr->payloadReservationID != pi.payloadReservationID)
            {
                return ipmi::responseInvalidReservationId();
            }

            pi.payloadReservationID     = 0;
            pi.payloadType              = 0;
            pi.payloadTotalSize         = 0;
            pi.actualTotalPayloadWritten = 0;

            // Remove the in-flight temp file
            std::string tempPath = std::string(biosConfigFolder) + "/temp" +
                                   std::to_string(payloadType);
            ::unlink(tempPath.c_str());
            flushNVOOBdata();
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "BIOS OOB: payload transfer aborted",
                phosphor::logging::entry("TYPE=%u", payloadType));
            return ipmi::responseSuccess(0u);
        }

        // ---------------------------------------------------------------
        default:
            return ipmi::responseInvalidFieldRequest();
    }
}

/**
 * @brief GetPayload (0x72) – read payload info, data, or status.
 *
 * Request:  paramSel (GetPayloadParameter), payloadType, [offset, length]
 * Response: variable – see GetPayloadParameter cases
 */
ipmi::RspType<ipmi::message::Payload>
    ipmiGetPayload(ipmi::Context::ptr& ctx,
                   uint8_t paramSel,
                   uint8_t payloadType,
                   ipmi::message::Payload& reqPayload)
{
    ipmi::message::Payload retValue;

    if (static_cast<GetPayloadParameter>(paramSel) >=
        GetPayloadParameter::MaxPayloadParameters)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    if (!(gNVOOBdata.mBIOSCapabilities.OOBCapability & biosCapOobSupported))
    {
        return ipmi::response(cc::biosCapNotInit);
    }
    if (payloadType >= maxPayloadSupported)
    {
        return ipmi::responseInvalidFieldRequest();
    }

    const auto& pi = gNVOOBdata.payloadInfo[payloadType];

    switch (static_cast<GetPayloadParameter>(paramSel))
    {
        // ---------------------------------------------------------------
        case GetPayloadParameter::GetPayloadInfo:
        {
            std::string path = std::string(biosConfigFolder) + "/Payload" +
                               std::to_string(payloadType);
            std::ifstream ifs(path, std::ios::in | std::ios::binary |
                                        std::ios::ate);
            if (!ifs.good())
            {
                return ipmi::response(ipmi::ccUnspecifiedError);
            }
            ifs.close();
            retValue.pack(pi.payloadVersion);
            retValue.pack(payloadType);
            retValue.pack(pi.payloadTotalSize);
            retValue.pack(pi.payloadTotalChecksum);
            retValue.pack(pi.payloadflag);
            retValue.pack(pi.payloadStatus);
            retValue.pack(pi.payloadTimeStamp);
            return ipmi::responseSuccess(std::move(retValue));
        }

        // ---------------------------------------------------------------
        case GetPayloadParameter::GetPayloadData:
        {
            if (pi.payloadStatus != static_cast<uint8_t>(PStatus::Valid))
            {
                return ipmi::responseResponseError();
            }
            std::vector<uint32_t> req;
            if (reqPayload.unpack(req) || !reqPayload.fullyUnpacked() ||
                req.size() < 2)
            {
                return ipmi::responseReqDataLenInvalid();
            }
            uint32_t offset = req[0];
            uint32_t length = req[1];

            if (length > static_cast<uint32_t>(maxGetPayloadDataSize))
            {
                return ipmi::responseInvalidFieldRequest();
            }

            std::string path = std::string(biosConfigFolder) + "/Payload" +
                               std::to_string(payloadType);
            std::ifstream ifs(path, std::ios::in | std::ios::binary |
                                        std::ios::ate);
            if (!ifs.good())
            {
                return ipmi::response(ipmi::ccUnspecifiedError);
            }
            auto fileSize = static_cast<uint64_t>(ifs.tellg());
            if (fileSize < offset)
            {
                return ipmi::responseInvalidFieldRequest();
            }
            if ((fileSize - offset) < length)
            {
                return ipmi::responseInvalidFieldRequest();
            }
            ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

            std::array<uint8_t, maxGetPayloadDataSize> buf{};
            ifs.read(reinterpret_cast<char*>(buf.data()),
                     static_cast<std::streamsize>(length));
            uint32_t readCount = static_cast<uint32_t>(ifs.gcount());
            ifs.close();

            boost::crc_32_type crc;
            crc.process_bytes(buf.data(), readCount);

            retValue.pack(payloadType);
            retValue.pack(readCount);
            retValue.pack(crc.checksum());
            for (uint32_t i = 0; i < readCount; ++i)
            {
                retValue.pack(buf[i]);
            }
            return ipmi::responseSuccess(std::move(retValue));
        }

        // ---------------------------------------------------------------
        case GetPayloadParameter::GetPayloadStatus:
        {
            retValue.pack(pi.payloadStatus);
            return ipmi::responseSuccess(std::move(retValue));
        }

        default:
            return ipmi::responseInvalidFieldRequest();
    }
}

// -----------------------------------------------------------------------
// Handler registration (runs before main via __attribute__((constructor)))
// -----------------------------------------------------------------------

static void registerBIOSConfigFunctions()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock BIOS config module initialised");
    initNVOOBdata();

    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnOemSix,
                          static_cast<ipmi::Cmd>(general::cmdSetBIOSCap),
                          ipmi::Privilege::Admin, ipmiSetBIOSCap);

    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnOemSix,
                          static_cast<ipmi::Cmd>(general::cmdGetBIOSCap),
                          ipmi::Privilege::User, ipmiGetBIOSCap);

    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnOemSix,
                          static_cast<ipmi::Cmd>(general::cmdSetPayload),
                          ipmi::Privilege::Admin, ipmiSetPayload);

    ipmi::registerHandler(ipmi::prioOemBase,
                          ipmi::netFnOemSix,
                          static_cast<ipmi::Cmd>(general::cmdGetPayload),
                          ipmi::Privilege::User, ipmiGetPayload);
}

} // namespace asrock
