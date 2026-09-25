#include <iostream>

#include "uwb/client/version.hpp"

int main() {
    // Phase 0 placeholder: the uwbctl CLI arrives in implementation plan §25.
    std::cout << uwb::client::clientCoreVersion() << "\n";
    return 0;
}
