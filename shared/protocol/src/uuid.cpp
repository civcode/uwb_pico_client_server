#include "uwb/protocol/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uwb::protocol {

namespace {

// Self-contained SHA-1 (RFC 3174) implementation.
//
// The protocol library must not depend on any third-party or platform library
// (implementation plan §12 acceptance), so SHA-1 required by UUIDv5 is included
// here instead of pulling in OpenSSL/mbedTLS.
struct Sha1 {
    std::uint32_t state[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    std::array<std::uint8_t, 64> block{};
    std::size_t blockLen = 0;
    std::uint64_t totalLen = 0;

    static std::uint32_t rotateLeft(std::uint32_t value, unsigned bits) noexcept {
        return (value << bits) | (value >> (32U - bits));
    }

    void transform(const std::uint8_t *data) {
        std::uint32_t w[80] = {};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(data[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];

        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }

            const std::uint32_t temp = rotateLeft(a, 5U) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30U);
            b = a;
            a = temp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    void update(const std::uint8_t *data, std::size_t len) {
        totalLen += len;
        while (len > 0) {
            const std::size_t take = len < (64 - blockLen) ? len : (64 - blockLen);
            std::memcpy(block.data() + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == 64) {
                transform(block.data());
                blockLen = 0;
            }
        }
    }

    std::array<std::uint8_t, 20> finalize() {
        const std::uint64_t bitLen = totalLen * 8ULL;

        const std::uint8_t pad = 0x80;
        update(&pad, 1);

        const std::uint8_t zero = 0x00;
        while (blockLen != 56) {
            update(&zero, 1);
        }

        std::array<std::uint8_t, 8> lengthBytes{};
        for (int i = 0; i < 8; ++i) {
            lengthBytes[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((bitLen >> static_cast<unsigned>(56 - i * 8)) & 0xFFU);
        }
        update(lengthBytes.data(), 8);

        std::array<std::uint8_t, 20> digest{};
        for (int i = 0; i < 5; ++i) {
            digest[static_cast<std::size_t>(i * 4)] = static_cast<std::uint8_t>(state[i] >> 24U);
            digest[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>(state[i] >> 16U);
            digest[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(state[i] >> 8U);
            digest[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(state[i]);
        }
        return digest;
    }
};

std::array<std::uint8_t, 20> sha1(ConstBytes data) {
    Sha1 sha;
    if (!data.empty()) {
        sha.update(data.data(), data.size());
    }
    return sha.finalize();
}

constexpr int hexNibble(std::uint8_t c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

bool Uuid::isZero() const noexcept {
    for (std::uint8_t b : bytes) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

void Uuid::formatLowerHex(char *out, std::size_t outSize) const noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    static constexpr std::size_t kTextSize = 36;

    if (out == nullptr || outSize < kTextSize + 1) {
        return;
    }

    std::size_t pos = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[pos++] = '-';
        }
        out[pos++] = kHex[(bytes[i] >> 4U) & 0x0FU];
        out[pos++] = kHex[bytes[i] & 0x0FU];
    }
    out[pos] = '\0';
}

Result<Uuid> uuidFromString(std::string_view text) noexcept {
    if (text.size() != 36) {
        return Result<Uuid>::error(ProtocolErrorCode::InvalidField,
                                   static_cast<std::uint32_t>(text.size()));
    }

    Uuid uuid;
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return Result<Uuid>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(i));
            }
            continue;
        }

        const int high = hexNibble(static_cast<std::uint8_t>(c));
        if (high < 0) {
            return Result<Uuid>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(i));
        }
        if (i + 1 >= text.size()) {
            return Result<Uuid>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(i));
        }
        const int low = hexNibble(static_cast<std::uint8_t>(text[i + 1]));
        if (low < 0) {
            return Result<Uuid>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(i + 1));
        }

        uuid.bytes[out++] = static_cast<std::uint8_t>(((high << 4) | low) & 0xFF);
        ++i; // consume the second nibble
    }

    return Result<Uuid>::ok(uuid);
}

Uuid uuidFromU64BigEndian(std::uint64_t value) noexcept {
    Uuid uuid;
    for (std::size_t i = 0; i < 8; ++i) {
        const unsigned shift = static_cast<unsigned>(56U - i * 8U);
        uuid.bytes[8U + i] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
    return uuid;
}

const Uuid &deviceUuidNamespace() noexcept {
    // 18659371-d91d-42d0-a58d-13b10305bbee
    static const Uuid kNamespace{{0x18, 0x65, 0x93, 0x71, 0xd9, 0x1d, 0x42, 0xd0,
                                 0xa5, 0x8d, 0x13, 0xb1, 0x03, 0x05, 0xbb, 0xee}};
    return kNamespace;
}

Uuid uuidV5(const Uuid &namespaceUuid, ConstBytes name) noexcept {
    ByteBuffer joined;
    joined.reserve(namespaceUuid.bytes.size() + name.size());
    joined.insert(joined.end(), namespaceUuid.bytes.begin(), namespaceUuid.bytes.end());
    joined.insert(joined.end(), name.begin(), name.end());

    const auto digest = sha1(bytesOf(joined));

    Uuid uuid;
    for (std::size_t i = 0; i < 16; ++i) {
        uuid.bytes[i] = digest[i];
    }

    // RFC 9562: version 5 in the high nibble of byte 6, variant 10b in byte 8.
    uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0FU) | 0x50U);
    uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3FU) | 0x80U);
    return uuid;
}

Result<Uuid> deviceUuidFromBoardId(ConstBytes boardId) noexcept {
    if (boardId.size() != 8) {
        return Result<Uuid>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(boardId.size()));
    }

    static constexpr char kHex[] = "0123456789abcdef";

    // Name input: 16 lowercase hexadecimal ASCII characters (specification §6.2).
    std::array<char, 16> name{};
    for (std::size_t i = 0; i < 8; ++i) {
        name[i * 2] = kHex[(boardId[i] >> 4U) & 0x0FU];
        name[i * 2 + 1] = kHex[boardId[i] & 0x0FU];
    }

    ConstBytes nameBytes{reinterpret_cast<const std::uint8_t *>(name.data()), name.size()};
    return Result<Uuid>::ok(uuidV5(deviceUuidNamespace(), nameBytes));
}

Uuid deviceUuidFromBoardIdU64(std::uint64_t boardId) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((boardId >> static_cast<unsigned>(56 - i * 8)) & 0xFFU);
    }
    ConstBytes span{bytes.data(), bytes.size()};
    auto result = deviceUuidFromBoardId(span);
    return result.value();
}

} // namespace uwb::protocol
