#include "uwb/protocol/version.hpp"

#include <string_view>

namespace uwb::protocol {

std::string_view libraryVersion() noexcept {
    return "uwb_protocol 0.1.0 (wire protocol v1)";
}

} // namespace uwb::protocol
