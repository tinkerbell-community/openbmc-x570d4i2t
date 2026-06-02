// SPDX-License-Identifier: Apache-2.0
//
// phosphor-ipmi-blobs handler at /ami/smbios.
//
// The blob accepts an AMI Aptio V buffer (leading 4-byte AmiMdrHeader is
// optional and stripped by the converter). On commit the converter writes a
// proper MDR V2 file at /var/lib/smbios/smbios2 and triggers
// AgentSynchronizeData on smbios-mdrv2. Reads serve the AMI Aptio V view of
// the current cache.

#include "handler.hpp"
#include "converter.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace blobs
{

bool AmiSmbiosBlobHandler::canHandleBlob(const std::string& path)
{
    return path == blobId;
}

std::vector<std::string> AmiSmbiosBlobHandler::getBlobIds()
{
    return std::vector<std::string>(1, blobId);
}

bool AmiSmbiosBlobHandler::deleteBlob(const std::string& /* path */)
{
    return false;
}

bool AmiSmbiosBlobHandler::stat(const std::string& path,
                                struct BlobMeta* meta)
{
    if (!blobPtr || blobPtr->blobId != path)
    {
        return false;
    }
    meta->size = blobPtr->buffer.size();
    meta->blobState = blobPtr->state;
    return true;
}

bool AmiSmbiosBlobHandler::open(uint16_t session, uint16_t flags,
                                const std::string& path)
{
    if (!canHandleBlob(path))
    {
        return false;
    }
    if (blobPtr)
    {
        return false;
    }
    blobPtr = std::make_unique<AmiSmbiosBlob>(session, path, flags);
    if (flags & blobs::OpenFlags::read)
    {
        blobPtr->state |= blobs::StateFlags::open_read;
        blobPtr->buffer = ami::amiViewFromCache();
    }
    return true;
}

std::vector<uint8_t> AmiSmbiosBlobHandler::read(uint16_t session,
                                                uint32_t offset,
                                                uint32_t requestedSize)
{
    if (!blobPtr || blobPtr->sessionId != session)
    {
        return {};
    }
    if (!(blobPtr->state & blobs::StateFlags::open_read))
    {
        return {};
    }
    if (offset >= blobPtr->buffer.size())
    {
        return {};
    }
    uint32_t remain = static_cast<uint32_t>(blobPtr->buffer.size()) - offset;
    uint32_t n = std::min(remain, requestedSize);
    return std::vector<uint8_t>(blobPtr->buffer.begin() + offset,
                                blobPtr->buffer.begin() + offset + n);
}

bool AmiSmbiosBlobHandler::write(uint16_t session, uint32_t offset,
                                 const std::vector<uint8_t>& data)
{
    if (!blobPtr || blobPtr->sessionId != session)
    {
        return false;
    }
    if (!(blobPtr->state & blobs::StateFlags::open_write))
    {
        lg2::error("No open blob to write");
        return false;
    }
    if (offset >= maxBufferSize)
    {
        return false;
    }
    uint32_t remain = maxBufferSize - offset;
    if (data.size() > remain)
    {
        return false;
    }
    uint32_t newSize = static_cast<uint32_t>(data.size()) + offset;
    if (newSize > blobPtr->buffer.size())
    {
        blobPtr->buffer.resize(newSize);
    }
    std::memcpy(blobPtr->buffer.data() + offset, data.data(), data.size());
    return true;
}

bool AmiSmbiosBlobHandler::writeMeta(uint16_t /* session */,
                                     uint32_t /* offset */,
                                     const std::vector<uint8_t>& /* data */)
{
    return false;
}

bool AmiSmbiosBlobHandler::commit(uint16_t session,
                                  const std::vector<uint8_t>& data)
{
    if (!data.empty())
    {
        lg2::error("Unexpected data provided to commit call");
        return false;
    }
    if (!blobPtr || blobPtr->sessionId != session)
    {
        return false;
    }
    if (blobPtr->state &
        (blobs::StateFlags::committing | blobs::StateFlags::committed))
    {
        return true;
    }
    blobPtr->state &= ~blobs::StateFlags::commit_error;

    if (!ami::persistAmiBuffer(blobPtr->buffer.data(), blobPtr->buffer.size()))
    {
        blobPtr->state |= blobs::StateFlags::commit_error;
        return false;
    }
    blobPtr->state |= blobs::StateFlags::committing;

    if (!ami::triggerMdrSync())
    {
        blobPtr->state &= ~blobs::StateFlags::committing;
        blobPtr->state |= blobs::StateFlags::commit_error;
        return false;
    }

    blobPtr->state &= ~blobs::StateFlags::committing;
    blobPtr->state |= blobs::StateFlags::committed;
    return true;
}

bool AmiSmbiosBlobHandler::close(uint16_t session)
{
    if (!blobPtr || blobPtr->sessionId != session)
    {
        return false;
    }
    blobPtr = nullptr;
    return true;
}

bool AmiSmbiosBlobHandler::stat(uint16_t session, struct BlobMeta* meta)
{
    if (!blobPtr || blobPtr->sessionId != session)
    {
        return false;
    }
    meta->size = blobPtr->buffer.size();
    meta->blobState = blobPtr->state;
    return true;
}

bool AmiSmbiosBlobHandler::expire(uint16_t session)
{
    return close(session);
}

void setupAmiSmbiosHandler() __attribute__((constructor));
void setupAmiSmbiosHandler()
{
    ami::seedFromBakedIfMissing();
}

} // namespace blobs
