#include "conga.h"

#include <stdexcept>
#include <set>

RoutePath
CongaLoadBalancer::selectRoute(const RouteContext &context)
{
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
        // SplitMix64 finalizer with fixed-width unsigned arithmetic. Unlike
        // std::hash, this mapping is explicitly defined and independent of calls.
        auto mix = [](uint64_t value) {
            value += UINT64_C(0x9e3779b97f4a7c15);
            value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
            return value ^ (value >> 31);
        };
        const uint64_t endpoints = (uint64_t(context.srcHost) << 32) | context.dstHost;
        const uint64_t flowHash = mix(endpoints) ^ mix(context.flowId) ^ mix(_routingSeed);
        const RoutePath *selected = nullptr;
        uint32_t bestMetric = 8;
        uint64_t bestRank = 0;
        std::set<PathId> seenPaths;
        std::set<uint32_t> seenUplinks;
        for (const auto &candidate : context.paths) {
            const auto mapping = context.congestion.find(candidate.pathId);
            if (mapping == context.congestion.end()) {
                throw std::invalid_argument("CONGA candidate has no source-uplink measurement");
            }
            const CandidateCongestion &measurement = mapping->second;
            if (!seenPaths.insert(candidate.pathId).second ||
                !seenUplinks.insert(measurement.sourceUplink).second) {
                throw std::invalid_argument("CONGA candidates require unique path IDs and source uplinks");
            }
            if (measurement.sourceUplink >= 16 || measurement.localMetric >= 8 ||
                measurement.remoteMetric >= 8) {
                throw std::invalid_argument("CONGA uplinks require 4-bit tags and 3-bit metrics");
            }
            // Unknown/stale remote state contributes zero: local measurement
            // still applies, and unobserved paths remain eligible for exploration.
            const SelectionMetrics metrics = measurement.measurements();
            const uint32_t metric = metrics.score;
            // Stable per-flow rank spreads ties without consuming traffic RNGs.
            // Rank by PathId, so reordering the candidate list cannot change ties.
            const uint64_t rank = mix(flowHash ^ mix(candidate.pathId.value));
            if (selected == nullptr || metric < bestMetric ||
                (metric == bestMetric && (rank < bestRank ||
                 (rank == bestRank && candidate.pathId < selected->pathId)))) {
                selected = &candidate;
                route.metrics = metrics;
                bestMetric = metric;
                bestRank = rank;
            }
        }
        const RoutePath &path = *selected;
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
