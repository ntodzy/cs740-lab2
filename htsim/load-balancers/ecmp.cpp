#include "ecmp.h"

#include <cstdlib>
#include <stdexcept>

EcmpLoadBalancer::EcmpLoadBalancer(uint32_t uplinkCount)
    : _uplinkCount(uplinkCount)
{
    if (_uplinkCount == 0) {
        throw std::invalid_argument("ECMP requires at least one uplink");
    }
}

uint32_t
EcmpLoadBalancer::chooseUplink(uint32_t, uint32_t)
{
    // Preserve the existing baseline: random equal-cost selection per flow,
    // rather than hashing a flow tuple. The route remains fixed afterward.
    return std::rand() % _uplinkCount;
}
