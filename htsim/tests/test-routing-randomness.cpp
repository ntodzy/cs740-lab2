#include "../load-balancers/ecmp.h"

#include <cstdlib>
#include <iostream>

int main()
{
    std::srand(12314);
    const int expectedTrafficDraw = std::rand();
    std::srand(12314);

    EcmpLoadBalancer first(12, 42);
    EcmpLoadBalancer repeat(12, 42);
    EcmpLoadBalancer different(12, 43);
    bool differs = false;
    unsigned int counts[12] = {};
    for (unsigned int i = 0; i < 120000; i++) {
        const uint32_t choice = first.chooseUplink(i % 768, (i + 37) % 768, i);
        // Unrelated calls must not affect a flow's core mapping.
        repeat.chooseUplink(500, 600, i + 900000);
        if (choice >= 12 || choice != repeat.chooseUplink(i % 768, (i + 37) % 768, i)) {
            std::cerr << "Invalid or nonrepeatable uplink choice\n";
            return 1;
        }
        ++counts[choice];
        differs |= choice != different.chooseUplink(i % 768, (i + 37) % 768, i);
    }
    if (!differs || std::rand() != expectedTrafficDraw) {
        std::cerr << "Routing seeds are ineffective or routing consumed traffic randomness\n";
        return 1;
    }
    for (unsigned int core = 0; core < 12; ++core) {
        // A generous deterministic regression bound around 10,000 per core.
        if (counts[core] < 9200 || counts[core] > 10800) {
            std::cerr << "Unbalanced ECMP distribution on core " << core << '\n';
            return 1;
        }
        std::cout << "Core " << core << ": " << counts[core] << " flows\n";
    }
    std::cout << "Hash stability, 12-core distribution, and traffic RNG isolation passed\n";
}
