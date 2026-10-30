#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include <cstdint>
#include <random>

// Shared interface for uplink policies; route construction stays shared.
class LoadBalancer
{
public:
    explicit LoadBalancer(uint32_t routingSeed)
        : _routingSeed(routingSeed), _routingRng(routingSeed) {}
    virtual ~LoadBalancer() = default;
    virtual uint32_t chooseUplink(uint32_t srcHost, uint32_t dstHost,
                                  uint64_t flowId) = 0;

protected:
    uint32_t _routingSeed;
    // All routing and tie-breaking draws must use this engine, never rand()
    // or drand(): those belong to traffic generation in experiment 2.
    std::mt19937 _routingRng;
};

#endif // LOAD_BALANCER_H
