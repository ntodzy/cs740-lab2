#include "conga.h"

#include <stdexcept>

RoutePath
CongaLoadBalancer::selectRoute(const RouteContext &context)
{
    // TODO (step 7): replace ECMP selection with congestion-aware selection.
    if (context.usesFabric && context.paths.empty()) {
        throw std::invalid_argument("Fabric routing requires at least one available path");
    }
    RoutePath route{};
    route.pathId = context.directPathId;
    route_t *fwd = &route.forward;
    route_t *rev = &route.reverse;
    fwd->insert(fwd->end(), context.forward.access.begin(), context.forward.access.end());
    rev->insert(rev->end(), context.reverse.access.begin(), context.reverse.access.end());

    // LOAD BALANCER: choose among the topology's available forwarding options.
    // Local traffic needs no fabric path selection.
    if (context.usesFabric) {
        // The ECMP placeholder keeps both modes identical until CONGA selection is implemented.
        // SplitMix64 finalizer with fixed-width unsigned arithmetic. Unlike
        // std::hash, this mapping is explicitly defined and independent of calls.
        auto mix = [](uint64_t value) {
            value += UINT64_C(0x9e3779b97f4a7c15);
            value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
            return value ^ (value >> 31);
        };
        uint64_t endpoints = (uint64_t(context.srcHost) << 32) | context.dstHost;
        const size_t selectedPath = mix(mix(endpoints) ^ mix(context.flowId) ^ mix(_routingSeed))
            % context.paths.size();
        const RoutePath &path = context.paths.at(selectedPath);
        route.pathId = path.pathId; // An opaque ID, not a core or candidate index.

        // Install the selected path for all data packets and reverse ACKs
        // in this flow (the lab treats a whole flow as one flowlet).
        // The topology supplies ordered hops, including its tagging/feedback hooks.

        // Forward data path: append the chosen option's forward execution hops.
        fwd->insert(fwd->end(), path.forward.begin(), path.forward.end());

        // Reverse ACK path: append the chosen option's reverse execution hops.
        rev->insert(rev->end(), path.reverse.begin(), path.reverse.end());
    }

    fwd->insert(fwd->end(), context.forward.delivery.begin(), context.forward.delivery.end());
    rev->insert(rev->end(), context.reverse.delivery.begin(), context.reverse.delivery.end());
    return route;
}
