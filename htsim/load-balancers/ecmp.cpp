#include "ecmp.h"

#include <stdexcept>

RoutePath
EcmpLoadBalancer::selectRoute(const RouteContext &context)
{
    if (context.usesFabric && context.paths.empty()) {
        throw std::invalid_argument("Fabric routing requires at least one available path");
    }
    RoutePath route{};
    route.pathId = context.directPathId;
    route_t *fwd = &route.forward;
    route_t *rev = &route.reverse;
    const RoutePath *selected = nullptr;

    // LOAD BALANCER: choose among the topology's available forwarding options.
    // Local traffic needs no fabric path selection.
    if (context.usesFabric) {
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
        selected = &path;
        // Congestion is optional for ECMP. Carry supplied observations for
        // reporting without making them a prerequisite for selecting a path.
        if (selectedPath < context.congestion.size() && context.congestion[selectedPath].available) {
            route.metrics = context.congestion[selectedPath].measurements();
        }
        route.pathId = path.pathId; // An opaque ID, not a core or candidate index.
    }

    // Build the complete forward and reverse routes, including access and delivery hops.
    // We reserve the total size to avoid repeated reallocations/resize, then append each segment.
    fwd->reserve(context.forward.access.size() + context.forward.delivery.size()
                 + (selected ? selected->forward.size() : 0));
    rev->reserve(context.reverse.access.size() + context.reverse.delivery.size()
                 + (selected ? selected->reverse.size() : 0));
    fwd->insert(fwd->end(), context.forward.access.begin(), context.forward.access.end());
    rev->insert(rev->end(), context.reverse.access.begin(), context.reverse.access.end());
    if (selected) {
        const RoutePath &path = *selected;

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
