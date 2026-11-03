#ifndef CONGA_H
#define CONGA_H

#include "load-balancer.h"

// Select the least-congested candidate using local DRE and returned leaf feedback.
class CongaLoadBalancer : public LoadBalancer
{
public:
    explicit CongaLoadBalancer(uint32_t routingSeed = 1729)
        : LoadBalancer(routingSeed) {}
    RoutePath selectRoute(const RouteContext &context) override;
};

#endif // CONGA_H
