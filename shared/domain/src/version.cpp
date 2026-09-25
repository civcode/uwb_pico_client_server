#include "uwb/domain/version.hpp"

#include <string_view>

namespace uwb::domain {

std::string_view domainLibraryVersion() noexcept {
    return "uwb_domain 0.1.0";
}

} // namespace uwb::domain
