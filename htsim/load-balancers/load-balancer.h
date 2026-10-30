#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include <cstdint>

// Shared interface for uplink policies; route construction stays shared.
class LoadBalancer
{
public:
    virtual ~LoadBalancer() = default;
    virtual uint32_t chooseUplink(uint32_t srcLeaf, uint32_t dstLeaf) = 0;
};

#endif // LOAD_BALANCER_H
