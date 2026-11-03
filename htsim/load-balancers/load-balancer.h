#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include <cstdint>
#include <random>
#include "route-context.h"

// Each policy constructs the forward/reverse execution paths from available hops.
class LoadBalancer
{
public:
    explicit LoadBalancer(uint32_t routingSeed)
        : _routingSeed(routingSeed), _routingRng(routingSeed) {}
    virtual ~LoadBalancer() = default;
    // Return complete forward/reverse routes and their topology-defined path ID.
    virtual RoutePath selectRoute(const RouteContext &context) = 0;

protected:
    uint32_t _routingSeed;
    // All routing and tie-breaking draws must use this engine, never rand()
    // or drand(): those belong to traffic generation in experiment 2.
    std::mt19937 _routingRng;
};

#endif // LOAD_BALANCER_H
