// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ami:: converter implementation — MDR V2 on-disk ↔ AMI Aptio V over-wire.

#include <amiconverter.hpp>

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ami
{

uint16_t computeChecksum(const uint8_t* data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += data[i];
    return static_cast<uint16_t>(sum & 0xFFFF);
}

std::vector<uint8_t> loadMdrPayload()
{
    std::ifstream f(kSmbiosFile, std::ios::binary);
    if (!f.is_open())
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ami::loadMdrPayload: cannot open",
            phosphor::logging::entry("PATH=%s", kSmbiosFile));
        return {};
    }
    std::vector<uint8_t> raw(std::istreambuf_iterator<char>(f), {});
    if (raw.size() <= sizeof(MDRSMBIOSHeader))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ami::loadMdrPayload: file too small",
            phosphor::logging::entry("BYTES=%zu", raw.size()));
        return {};
    }
    return std::vector<uint8_t>(raw.begin() + sizeof(MDRSMBIOSHeader),
                                raw.end());
}

std::vector<uint8_t> amiViewFromCache()
{
    auto payload = loadMdrPayload();
    if (payload.empty())
        return {};
    AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(payload.size());
    hdr.checksum = computeChecksum(payload.data(), payload.size());
    std::vector<uint8_t> result(sizeof(hdr) + payload.size());
    std::memcpy(result.data(), &hdr, sizeof(hdr));
    std::memcpy(result.data() + sizeof(hdr), payload.data(), payload.size());
    return result;
}

bool writeMdrFile(const uint8_t* payload, size_t len)
{
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(kSmbiosFile).parent_path(), ec);
    if (ec)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ami::writeMdrFile: mkdir failed",
            phosphor::logging::entry("ERROR=%s", ec.message().c_str()));
        return false;
    }

    MDRSMBIOSHeader hdr{};
    hdr.dirVersion = kMdrDirVersion;
    hdr.mdrType    = kMdrTypeII;
    hdr.timestamp  = static_cast<uint32_t>(std::time(nullptr));
    hdr.dataSize   = static_cast<uint32_t>(len);

    std::ofstream out(kSmbiosFile,
                      std::ios_base::binary | std::ios_base::trunc);
    if (!out.is_open())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ami::writeMdrFile: open for write failed",
            phosphor::logging::entry("PATH=%s", kSmbiosFile));
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr),
              static_cast<std::streamsize>(sizeof(hdr)));
    out.write(reinterpret_cast<const char*>(payload),
              static_cast<std::streamsize>(len));
    out.close();
    if (out.fail())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ami::writeMdrFile: write failed",
            phosphor::logging::entry("PATH=%s", kSmbiosFile));
        return false;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ami::writeMdrFile: wrote",
        phosphor::logging::entry("TOTAL_BYTES=%zu", sizeof(hdr) + len));
    return true;
}

bool persistAmiBuffer(const uint8_t* amiBytes, size_t len)
{
    if (len <= sizeof(AmiMdrHeader))
        return writeMdrFile(amiBytes, len);

    AmiMdrHeader hdr{};
    std::memcpy(&hdr, amiBytes, sizeof(hdr));

    size_t      payloadLen = len - sizeof(AmiMdrHeader);
    const auto* payloadPtr = amiBytes + sizeof(AmiMdrHeader);

    if (hdr.dataSize == payloadLen)
    {
        uint16_t calc = computeChecksum(payloadPtr, payloadLen);
        if (calc == hdr.checksum)
            return writeMdrFile(payloadPtr, payloadLen);
    }

    uint16_t declaredSize = hdr.dataSize; // copy from packed field before logging
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "ami::persistAmiBuffer: AMI header validation failed — writing raw",
        phosphor::logging::entry("DECLARED=%u", declaredSize),
        phosphor::logging::entry("ACTUAL=%zu", payloadLen));
    return writeMdrFile(amiBytes, len);
}

bool triggerMdrSync()
{
    try
    {
        auto dbus = getSdBus();
        // Check if the MDR V2 service exists before making a blocking call.
        // ipmi::getService() throws if the service is not found, and that
        // exception is cheap (a NameHasNoOwner D-Bus error, not a 25s timeout).
        std::string service =
            ipmi::getService(*dbus, "xyz.openbmc_project.Smbios.MDR_V2",
                             "/xyz/openbmc_project/Smbios/MDR_V2");
        if (service.empty())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "ami::triggerMdrSync: smbios-mdrv2 service not found, skipping");
            return false;
        }
        sdbusplus::message_t method =
            dbus->new_method_call(service.c_str(),
                                  "/xyz/openbmc_project/Smbios/MDR_V2",
                                  "xyz.openbmc_project.Smbios.MDR_V2",
                                  "AgentSynchronizeData");
        bool ok = false;
        sdbusplus::message_t reply = dbus->call(method);
        reply.read(ok);
        return ok;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ami::triggerMdrSync: D-Bus call failed (service may not be ready)",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return false;
    }
}

void seedFromBakedIfMissing()
{
    std::error_code ec;
    if (std::filesystem::exists(kSmbiosFile, ec) && !ec)
    {
        auto sz = std::filesystem::file_size(kSmbiosFile, ec);
        if (!ec && sz > sizeof(MDRSMBIOSHeader))
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "ami::seedFromBakedIfMissing: smbios2 exists, skipping");
            return;
        }
    }

    std::ifstream src(kBakedDmp, std::ios::binary);
    if (!src.is_open())
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ami::seedFromBakedIfMissing: no baked dump",
            phosphor::logging::entry("PATH=%s", kBakedDmp));
        return;
    }
    std::vector<uint8_t> raw(std::istreambuf_iterator<char>(src), {});
    if (raw.size() <= sizeof(MDRSMBIOSHeader))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "ami::seedFromBakedIfMissing: baked dump too small",
            phosphor::logging::entry("BYTES=%zu", raw.size()));
        return;
    }

    std::error_code mkec;
    std::filesystem::create_directories(
        std::filesystem::path(kSmbiosFile).parent_path(), mkec);

    std::ofstream dst(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!dst.is_open())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ami::seedFromBakedIfMissing: cannot write smbios2",
            phosphor::logging::entry("PATH=%s", kSmbiosFile));
        return;
    }
    dst.write(reinterpret_cast<const char*>(raw.data()),
              static_cast<std::streamsize>(raw.size()));
    dst.close();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ami::seedFromBakedIfMissing: seeded from baked dump",
        phosphor::logging::entry("BYTES=%zu", raw.size()));
}

} // namespace ami
