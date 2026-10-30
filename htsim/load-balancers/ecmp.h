#ifndef ECMP_H
#define ECMP_H

#include "load-balancer.h"

// Stable seeded hash of endpoint IDs and the per-generator flow sequence.
class EcmpLoadBalancer : public LoadBalancer
{
public:
    explicit EcmpLoadBalancer(uint32_t uplinkCount, uint32_t routingSeed = 1729);
    uint32_t chooseUplink(uint32_t srcHost, uint32_t dstHost,
                          uint64_t flowId) override;

private:
    uint32_t _uplinkCount;
};

#endif // ECMP_H
