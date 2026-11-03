#ifndef CONGA_H
#define CONGA_H

#include "load-balancer.h"

// CONGA scaffold: independent execution path, currently using ECMP selection.
class CongaLoadBalancer : public LoadBalancer
{
public:
    explicit CongaLoadBalancer(uint32_t routingSeed = 1729)
        : LoadBalancer(routingSeed) {}
    RoutePath selectRoute(const RouteContext &context) override;
};

#endif // CONGA_H
