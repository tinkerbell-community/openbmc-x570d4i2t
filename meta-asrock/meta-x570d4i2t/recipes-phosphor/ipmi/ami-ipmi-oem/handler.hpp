// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <blobs-ipmid/blobs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace blobs
{

class AmiSmbiosBlobHandler : public GenericBlobInterface
{
  public:
    AmiSmbiosBlobHandler() = default;
    ~AmiSmbiosBlobHandler() override = default;
    AmiSmbiosBlobHandler(const AmiSmbiosBlobHandler&) = delete;
    AmiSmbiosBlobHandler& operator=(const AmiSmbiosBlobHandler&) = delete;
    AmiSmbiosBlobHandler(AmiSmbiosBlobHandler&&) = default;
    AmiSmbiosBlobHandler& operator=(AmiSmbiosBlobHandler&&) = default;

    struct AmiSmbiosBlob
    {
        AmiSmbiosBlob(uint16_t id, const std::string& path, uint16_t flags) :
            sessionId(id), blobId(path), state(0)
        {
            if (flags & blobs::OpenFlags::write)
            {
                state |= blobs::StateFlags::open_write;
            }
            buffer.reserve(maxBufferSize);
        }

        uint16_t sessionId;
        std::string blobId;
        uint16_t state;
        std::vector<uint8_t> buffer;
    };

    bool canHandleBlob(const std::string& path) override;
    std::vector<std::string> getBlobIds() override;
    bool deleteBlob(const std::string& path) override;
    bool stat(const std::string& path, struct BlobMeta* meta) override;
    bool open(uint16_t session, uint16_t flags,
              const std::string& path) override;
    std::vector<uint8_t> read(uint16_t session, uint32_t offset,
                              uint32_t requestedSize) override;
    bool write(uint16_t session, uint32_t offset,
               const std::vector<uint8_t>& data) override;
    bool writeMeta(uint16_t session, uint32_t offset,
                   const std::vector<uint8_t>& data) override;
    bool commit(uint16_t session, const std::vector<uint8_t>& data) override;
    bool close(uint16_t session) override;
    bool stat(uint16_t session, struct BlobMeta* meta) override;
    bool expire(uint16_t session) override;

  private:
    static constexpr char blobId[] = "/ami/smbios";
    static constexpr char bakedDump[] = "/usr/share/x570d4i2t/smbios.dmp";
    static constexpr uint32_t maxBufferSize = 64 * 1024;

    /* The handler only allows one open blob. */
    std::unique_ptr<AmiSmbiosBlob> blobPtr = nullptr;
};

} // namespace blobs
