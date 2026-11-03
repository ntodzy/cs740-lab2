#ifndef ECMP_H
#define ECMP_H

#include "load-balancer.h"

// Stable seeded hash of endpoint IDs and the per-generator flow sequence.
class EcmpLoadBalancer : public LoadBalancer
{
public:
    explicit EcmpLoadBalancer(uint32_t routingSeed = 1729)
        : LoadBalancer(routingSeed) {}
    RoutePath selectRoute(const RouteContext &context) override;
};

#endif // ECMP_H
