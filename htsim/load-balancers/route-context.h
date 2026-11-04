#ifndef ROUTE_CONTEXT_H
#define ROUTE_CONTEXT_H

#include "../packets/network.h"
#include <utility>

// Explicit construction prevents mixing candidate indices/core numbers with IDs.
struct PathId {
    PathId() : value(0) {}
    explicit PathId(uint32_t value) : value(value) {}
    bool operator==(PathId other) const { return value == other.value; }
    bool operator!=(PathId other) const { return !(*this == other); }
    bool operator<(PathId other) const { return value < other.value; }
    uint32_t value;
};

// Captured by selection and carried to observers; never reconstructed by logging.
struct SelectionMetrics {
    bool available = false;
    uint32_t sourceUplink = 0;
    uint32_t localMetric = 0;
    uint32_t remoteMetric = 0;
    bool remoteValid = false;
    uint32_t score = 0;
};

// An opaque identifier and ordered hops in both directions. Topology candidates
// contain fabric hops; selectRoute() returns complete access-to-delivery routes.
// PacketSink pointers are non-owning; the topology owns the actual objects.
struct RoutePath {
    RoutePath(PathId id = PathId(), route_t fwd = {}, route_t rev = {})
        : pathId(id), forward(std::move(fwd)), reverse(std::move(rev)) {}
    PathId pathId;
    route_t forward;
    route_t reverse;
    SelectionMetrics metrics;
};

struct RouteDirection {
    route_t access;
    route_t delivery;
};

// Immutable during selection: topology-supplied measurements at RouteContext::now.
// The uplink identifies the packet's LBTag and the source leaf's table column;
// it must not be inferred from the opaque PathId or the candidate's index.
struct CandidateCongestion {
    CandidateCongestion() : sourceUplink(0), localMetric(0), remoteMetric(0),
                            remoteValid(false), available(false) {}
    CandidateCongestion(uint32_t uplink, uint32_t local = 0,
                        uint32_t remote = 0, bool valid = false)
        : sourceUplink(uplink), localMetric(local), remoteMetric(remote), remoteValid(valid),
          available(true) {}
    uint32_t sourceUplink;
    uint32_t localMetric;
    uint32_t remoteMetric;
    bool remoteValid;
    bool available; // ECMP permits absent observations; CONGA requires them.

    // Shared definition of the measured path bottleneck, including cold start.
    SelectionMetrics measurements() const {
        SelectionMetrics result;
        result.available = available;
        result.sourceUplink = sourceUplink;
        result.localMetric = localMetric;
        result.remoteMetric = remoteValid ? remoteMetric : 0;
        result.remoteValid = remoteValid;
        result.score = result.localMetric > result.remoteMetric
            ? result.localMetric : result.remoteMetric;
        return result;
    }
};

struct RouteContext {
    explicit RouteContext(const std::vector<RoutePath> &availablePaths)
        : paths(availablePaths) {}
    uint32_t srcHost = 0;
    uint32_t dstHost = 0;
    uint32_t srcLeaf = 0;
    uint32_t dstLeaf = 0;
    uint64_t flowId = 0;
    simtime_picosec now = 0; // Timestamp shared by local and remote snapshots.
    // congestion[i] measures paths[i]. The index is only a local binding:
    // PathId stays opaque and sourceUplink remains an explicit table column.
    std::vector<CandidateCongestion> congestion;
    bool usesFabric = false; // Supplied by the topology, not inferred by a policy.
    PathId directPathId; // Topology-defined ID for routes without fabric hops.
    RouteDirection forward;
    RouteDirection reverse;
    const std::vector<RoutePath> &paths; // Must outlive route construction.
};

#endif // ROUTE_CONTEXT_H
