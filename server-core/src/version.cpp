#include "uwb/server/version.hpp"

#include <string_view>

namespace uwb::server {

std::string_view serverCoreVersion() noexcept {
    return "uwb_server_core 0.1.0";
}

} // namespace uwb::server
