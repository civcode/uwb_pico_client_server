#include <iostream>

#include "uwb/domain/version.hpp"
#include "uwb/protocol/version.hpp"
#include "uwb/server/version.hpp"

int main() {
    // Phase 0 placeholder: the simulator core arrives in implementation plan §4.
    std::cout << "uwb_simulator (placeholder)\n"
              << "  " << uwb::protocol::libraryVersion() << "\n"
              << "  " << uwb::domain::domainLibraryVersion() << "\n"
              << "  " << uwb::server::serverCoreVersion() << "\n";
    return 0;
}
