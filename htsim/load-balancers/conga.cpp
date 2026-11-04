#include "conga.h"

#include <stdexcept>
#include <array>

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
    const RoutePath *selected = nullptr;

    // LOAD BALANCER: choose among the topology's available forwarding options.
    // Local traffic needs no fabric path selection.
    if (context.usesFabric) {
        // SplitMix64 finalizer with fixed-width unsigned arithmetic. Unlike
        // std::hash, this mapping is explicitly defined and independent of calls.
        // Generates the same per-flow rank for both directions, so forward and reverse paths
        // are always consistent. The flow ID is included to avoid rank collisions
        // between different flows with the same endpoints. The routing seed is included
        // to allow reproducible experiments with different load-balancing policies.
        auto mix = [](uint64_t value) {
            value += UINT64_C(0x9e3779b97f4a7c15);
            value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
            return value ^ (value >> 31);
        };
        const uint64_t endpoints = (uint64_t(context.srcHost) << 32) | context.dstHost;
        const uint64_t flowHash = mix(endpoints) ^ mix(context.flowId) ^ mix(_routingSeed);

        // Select the least-congested candidate using local DRE and returned leaf feedback.
        uint32_t bestMetric = 8;
        uint64_t bestRank = 0;
        std::array<PathId, 16> seenPaths;
        size_t seenCount = 0;
        uint32_t seenUplinks = 0;
        for (size_t i = 0; i < context.paths.size(); ++i) {
            const auto &candidate = context.paths[i];
            // Check to see if the congesition entry exists
            if (i >= context.congestion.size() || !context.congestion[i].available) {
                throw std::invalid_argument("CONGA candidate has no source-uplink measurement");
            }

            // Check to see if the path ID and uplink are unique
            const CandidateCongestion &measurement = context.congestion[i];
            if (measurement.sourceUplink >= 16 || measurement.localMetric >= 8 ||
                measurement.remoteMetric >= 8) {
                throw std::invalid_argument("CONGA uplinks require 4-bit tags and 3-bit metrics");
            }

            // Preallocate a fixed-size array for seen paths and a bitmask for seen uplinks. This avoids dynamic memory allocation and ensures that the number of unique paths and uplinks is within expected limits.
            const uint32_t uplinkBit = uint32_t(1) << measurement.sourceUplink;
            bool duplicatePath = false;
            for (size_t j = 0; j < seenCount; ++j) {
                duplicatePath = duplicatePath || seenPaths[j] == candidate.pathId;
            }
            if (duplicatePath || (seenUplinks & uplinkBit)) {
                throw std::invalid_argument("CONGA candidates require unique path IDs and source uplinks");
            }
            // At most 16 unique four-bit uplinks can pass the checks above.
            seenPaths[seenCount++] = candidate.pathId;
            seenUplinks |= uplinkBit;

            // Unknown/stale remote state contributes zero: local measurement
            // still applies, and unobserved paths remain eligible for exploration.
            const SelectionMetrics metrics = measurement.measurements();
            const uint32_t metric = metrics.score;

            // Stable per-flow rank spreads ties without consuming traffic RNGs.
            // Rank by PathId, so reordering the candidate list cannot change ties.
            const uint64_t rank = mix(flowHash ^ mix(candidate.pathId.value));
            if (selected == nullptr || metric < bestMetric || // DRE minimizer
                (metric == bestMetric && (rank < bestRank || // Tie-breaker: stable per-flow rank
                  (rank == bestRank && candidate.pathId < selected->pathId)))) { // Tie-breaker: stable PathId order
                selected = &candidate;
                route.metrics = metrics;
                bestMetric = metric;
                bestRank = rank;
            }
        }
        route.pathId = selected->pathId; // An opaque ID, not a core or candidate index.
    }


    // Preallocate the forward and reverse route vectors to avoid repeated reallocations/resizes, then append each segment.
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
