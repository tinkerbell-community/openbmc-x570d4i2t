// SPDX-License-Identifier: Apache-2.0

#include "handler.hpp"

#include <blobs-ipmid/blobs.hpp>

#include <memory>

extern "C" std::unique_ptr<blobs::GenericBlobInterface> createHandler()
{
    return std::make_unique<blobs::AmiSmbiosBlobHandler>();
}
