#pragma once

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::Result;

// Persistent Pico-side configuration storage (specification §51).
//
// The stored payload is opaque to the server core: it is the 0xF01F Complete UWB
// Configuration TLV container (plus Pico-owned records added in a later phase).
// A write is staged first and committed second so a reset or power loss can never
// expose a half-written configuration (§51 two-phase rule).
class IConfigurationStorage {
public:
    virtual ~IConfigurationStorage() = default;

    // False when no persistent backend is available (e.g. host simulator).
    [[nodiscard]] virtual bool available() const noexcept = 0;

    // Read the active stored blob. Empty result when nothing is stored.
    [[nodiscard]] virtual Result<ByteBuffer> readActive() = 0;

    // Read the last known-good backup copy.
    [[nodiscard]] virtual Result<ByteBuffer> readBackup() = 0;

    // Stage a candidate blob. Nothing observable changes until commit().
    [[nodiscard]] virtual Result<bool> stage(ConstBytes candidate) = 0;

    // Swap the staged blob into the active slot, keeping the previous active
    // blob as the backup. Returns StorageUnavailable when no backend exists.
    [[nodiscard]] virtual Result<bool> commit() = 0;

    // Drop the staged blob without affecting the active one.
    virtual void discardStaged() noexcept = 0;

    // Restore the backup as the active blob (recovery path).
    [[nodiscard]] virtual Result<bool> restoreBackup() = 0;
};

} // namespace uwb::server
