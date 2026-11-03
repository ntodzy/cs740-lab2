#ifndef ROUTE_CONTEXT_H
#define ROUTE_CONTEXT_H

#include "../packets/network.h"
#include <map>

class LeafSwitch;

// Explicit construction prevents mixing candidate indices/core numbers with IDs.
struct PathId {
    PathId() : value(0) {}
    explicit PathId(uint32_t value) : value(value) {}
    bool operator==(PathId other) const { return value == other.value; }
    bool operator!=(PathId other) const { return !(*this == other); }
    bool operator<(PathId other) const { return value < other.value; }
    uint32_t value;
};

// An opaque identifier and ordered hops in both directions. Topology candidates
// contain fabric hops; selectRoute() returns complete access-to-delivery routes.
// PacketSink pointers are non-owning; the topology owns the actual objects.
struct RoutePath {
    PathId pathId;
    route_t forward;
    route_t reverse;
};

struct RouteDirection {
    route_t access;
    route_t delivery;
};

// Topology-supplied mapping and local measurement at RouteContext::now.
// The uplink identifies the packet's LBTag and the source leaf's table column;
// it must not be inferred from the opaque PathId or the candidate's index.
struct CandidateCongestion {
    uint32_t sourceUplink;
    uint32_t localMetric;
};

struct RouteContext {
    explicit RouteContext(const std::vector<RoutePath> &availablePaths)
        : paths(availablePaths) {}
    uint32_t srcHost = 0;
    uint32_t dstHost = 0;
    uint32_t srcLeaf = 0;
    uint32_t dstLeaf = 0;
    // Non-owning read-only access to live congestion tables. The source leaf
    // must outlive selection; ECMP ignores this congestion state.
    const LeafSwitch *sourceLeafState = nullptr;
    uint64_t flowId = 0;
    simtime_picosec now = 0; // One timestamp for local snapshots and remote reads.
    std::map<PathId, CandidateCongestion> congestion;
    bool usesFabric = false; // Supplied by the topology, not inferred by a policy.
    PathId directPathId; // Topology-defined ID for routes without fabric hops.
    RouteDirection forward;
    RouteDirection reverse;
    const std::vector<RoutePath> &paths; // Must outlive route construction.
};

#endif // ROUTE_CONTEXT_H
