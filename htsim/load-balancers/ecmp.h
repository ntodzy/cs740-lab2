#ifndef ECMP_H
#define ECMP_H

#include "load-balancer.h"

// Random equal-cost uplink selection, once per flow.
class EcmpLoadBalancer : public LoadBalancer
{
public:
    explicit EcmpLoadBalancer(uint32_t uplinkCount);
    uint32_t chooseUplink(uint32_t srcLeaf, uint32_t dstLeaf) override;

private:
    uint32_t _uplinkCount;
};

#endif // ECMP_H
