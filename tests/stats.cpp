#include "../src/stats.hpp"
#include <iostream>

int main() {
    paping::Statistics stats;
    stats.failure();
    stats.success(20, true);
    stats.success(40, false);
    stats.cancel();
    if (stats.attempted != 4 || stats.connected != 2 || stats.failed != 1 || stats.interrupted != 1
        || stats.minimum != 20 || stats.maximum != 40 || stats.average() != 30 || stats.close_incomplete != 1) {
        std::cerr << "Mixed failure/success/cancellation statistics are incorrect\n";
        return 1;
    }
    paping::Statistics empty;
    if (empty.average() != 0) return 1;
    return 0;
}
