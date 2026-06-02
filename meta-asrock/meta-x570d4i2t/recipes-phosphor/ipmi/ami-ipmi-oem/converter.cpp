// SPDX-License-Identifier: Apache-2.0

#include "converter.hpp"

#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <ipmid/api.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ami
{

namespace
{

constexpr const char* kMdrService   = "xyz.openbmc_project.Smbios.MDR_V2";
constexpr const char* kMdrPath      = "/xyz/openbmc_project/Smbios/MDR_V2";
constexpr const char* kMdrInterface = "xyz.openbmc_project.Smbios.MDR_V2";

#define LOG_INFO(fmt, ...) fprintf(stderr, "ami-converter [INFO]: " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "ami-converter [WARN]: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  fprintf(stderr, "ami-converter [ERR]:  " fmt "\n", ##__VA_ARGS__)

} // namespace

uint16_t computeChecksum(const uint8_t* data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        sum += data[i];
    }
    return static_cast<uint16_t>(sum & 0xFFFF);
}

std::vector<uint8_t> loadMdrPayload()
{
    std::vector<uint8_t> out;
    std::ifstream ifs(kSmbiosFile, std::ios::binary);
    if (!ifs)
    {
        return out;
    }
    MDRSMBIOSHeader hdr{};
    ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (ifs.gcount() != static_cast<std::streamsize>(sizeof(hdr)))
    {
        return out;
    }
    if (hdr.dirVersion != kMdrDirVersion || hdr.mdrType != kMdrTypeII)
    {
        LOG_WARN("MDR header mismatch (dirVer=0x%02X type=0x%02X)",
                 hdr.dirVersion, hdr.mdrType);
    }
    if (hdr.dataSize == 0 || hdr.dataSize > kMaxPayload)
    {
        return out;
    }
    out.resize(hdr.dataSize);
    ifs.read(reinterpret_cast<char*>(out.data()), hdr.dataSize);
    out.resize(static_cast<size_t>(ifs.gcount()));
    return out;
}

std::vector<uint8_t> amiViewFromCache()
{
    std::vector<uint8_t> payload = loadMdrPayload();
    std::vector<uint8_t> out(sizeof(AmiMdrHeader) + payload.size());
    AmiMdrHeader hdr{};
    hdr.dataSize = static_cast<uint16_t>(payload.size());
    hdr.checksum = computeChecksum(payload.data(), payload.size());
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (!payload.empty())
    {
        std::memcpy(out.data() + sizeof(hdr), payload.data(), payload.size());
    }
    return out;
}

bool writeMdrFile(const uint8_t* payload, size_t len)
{
    std::error_code ec;
    std::filesystem::create_directories(kSmbiosDir, ec);
    if (ec)
    {
        LOG_ERR("create %s failed: %s", kSmbiosDir, ec.message().c_str());
        return false;
    }

    std::ofstream ofs(kSmbiosFile, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        LOG_ERR("open %s for write failed", kSmbiosFile);
        return false;
    }

    MDRSMBIOSHeader hdr{};
    hdr.dirVersion = kMdrDirVersion;
    hdr.mdrType    = kMdrTypeII;
    hdr.timestamp  = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    hdr.dataSize   = static_cast<uint32_t>(len);

    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (payload && len)
    {
        ofs.write(reinterpret_cast<const char*>(payload),
                  static_cast<std::streamsize>(len));
    }
    return ofs.good();
}

bool persistAmiBuffer(const uint8_t* amiBytes, size_t len)
{
    if (!amiBytes || len == 0)
    {
        return writeMdrFile(nullptr, 0);
    }

    if (len >= sizeof(AmiMdrHeader))
    {
        AmiMdrHeader hdr{};
        std::memcpy(&hdr, amiBytes, sizeof(hdr));
        size_t payloadLen = len - sizeof(hdr);
        const uint8_t* payload = amiBytes + sizeof(hdr);

        if (hdr.dataSize == payloadLen)
        {
            uint16_t want = hdr.checksum;
            uint16_t got  = computeChecksum(payload, payloadLen);
            if (want != got)
            {
                LOG_WARN("AMI header checksum 0x%04X != computed 0x%04X "
                         "(persisting anyway)", want, got);
            }
            LOG_INFO("persistAmiBuffer: stripped 4-byte AMI header "
                     "(payload=%zu chk=0x%04X)", payloadLen, got);
            return writeMdrFile(payload, payloadLen);
        }
    }

    LOG_INFO("persistAmiBuffer: no AMI header detected, persisting raw "
             "(%zu bytes)", len);
    return writeMdrFile(amiBytes, len);
}

bool triggerMdrSync()
{
    sd_bus* bus = ipmid_get_sd_bus_connection();
    if (!bus)
    {
        LOG_ERR("triggerMdrSync: no sd-bus connection");
        return false;
    }

    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, kMdrService, kMdrPath, kMdrInterface,
                               "AgentSynchronizeData", &err, &reply, nullptr);
    bool ok = (r >= 0);
    if (!ok)
    {
        LOG_ERR("AgentSynchronizeData failed: %s",
                err.message ? err.message : strerror(-r));
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    return ok;
}

void seedFromBakedIfMissing()
{
    if (std::filesystem::exists(kSmbiosFile))
    {
        return;
    }
    std::ifstream ifs(kBakedDmp, std::ios::binary);
    if (!ifs)
    {
        LOG_WARN("baked dump %s not present; cache empty until first push",
                 kBakedDmp);
        return;
    }
    std::vector<uint8_t> baked((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
    if (baked.empty())
    {
        return;
    }
    if (writeMdrFile(baked.data(), baked.size()))
    {
        LOG_INFO("seeded %s from %s (%zu bytes)", kSmbiosFile, kBakedDmp,
                 baked.size());
        triggerMdrSync();
    }
}

} // namespace ami
