#include "ecmp.h"

#include <stdexcept>

EcmpLoadBalancer::EcmpLoadBalancer(uint32_t uplinkCount, uint32_t routingSeed)
    : LoadBalancer(routingSeed), _uplinkCount(uplinkCount)
{
    if (_uplinkCount == 0) {
        throw std::invalid_argument("ECMP requires at least one uplink");
    }
}

uint32_t
EcmpLoadBalancer::chooseUplink(uint32_t srcHost, uint32_t dstHost, uint64_t flowId)
{
    // SplitMix64 finalizer with fixed-width unsigned arithmetic. Unlike
    // std::hash, this mapping is explicitly defined and independent of calls.
    auto mix = [](uint64_t value) {
        value += UINT64_C(0x9e3779b97f4a7c15);
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    };
    uint64_t endpoints = (uint64_t(srcHost) << 32) | dstHost;
    return mix(mix(endpoints) ^ mix(flowId) ^ mix(_routingSeed)) % _uplinkCount;
}
