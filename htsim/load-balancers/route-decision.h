#ifndef ROUTE_DECISION_H
#define ROUTE_DECISION_H

#include "route-context.h"
#include <string>

// An immutable observation of an already completed routing decision. No live
// simulator state is reachable from it, and observers perform no scoring.
struct RouteDecision {
    RouteDecision(const std::string &mode, const RouteContext &context, const RoutePath &path)
        : flowId(context.flowId), time(context.now), mode(mode),
          source(context.srcHost), destination(context.dstHost),
          sourceLeaf(context.srcLeaf), destinationLeaf(context.dstLeaf),
          usesFabric(context.usesFabric), pathId(path.pathId), metrics(path.metrics) {}

    const uint64_t flowId;
    const simtime_picosec time;
    const std::string mode;
    const uint32_t source, destination, sourceLeaf, destinationLeaf;
    const bool usesFabric;
    const PathId pathId;
    const SelectionMetrics metrics;
};

#endif // ROUTE_DECISION_H
