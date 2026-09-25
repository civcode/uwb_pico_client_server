#include "uwb/client/version.hpp"

#include <string_view>

namespace uwb::client {

std::string_view clientCoreVersion() noexcept {
    return "uwb_client_core 0.1.0";
}

} // namespace uwb::client
