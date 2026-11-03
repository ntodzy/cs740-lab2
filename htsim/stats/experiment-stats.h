// AI-generated: created by OpenAI Codex.
#ifndef EXPERIMENT_STATS_H
#define EXPERIMENT_STATS_H

#include "../loggertypes.h"
#include "../flow-generator.h"
#include "../congestion/dre-queue-wrapper.h"
#include "../load-balancers/route-context.h"

#include <fstream>
#include <map>
#include <vector>
#include <tuple>

// Counters only: no periodic sampling or per-packet disk output.
class ExperimentStats : public QueueLogger
{
public:
    explicit ExperimentStats(const std::string &prefix);
    void registerLink(Queue &queue, linkspeed_bps speed, int core = -1,
                      DreQueueWrapper *dre = nullptr);
    void logQueue(Queue &queue, QueueEvent event, Packet &packet) override;
    uint64_t nextFlowId() const { return _routes.size(); }
    void recordRoute(uint32_t source, uint32_t destination, int core);
    void recordDecision(const std::string &mode, const RouteContext &context,
                        const RoutePath &path);
    void write(const FlowGenerator &generator, simtime_picosec endTime);

private:
    struct Link {
        std::string name;
        linkspeed_bps speed;
        int core;
        DreQueueWrapper *dre;
        uint64_t arrivals = 0, arrivalBytes = 0;
        uint64_t departures = 0, departureBytes = 0;
        uint64_t drops = 0, dropBytes = 0;
    };
    struct Route { uint32_t source, destination; int core; };
    struct Decision {
        uint64_t flowId;
        simtime_picosec time;
        std::string mode;
        uint32_t source, destination, sourceLeaf, destinationLeaf;
        bool usesFabric;
        PathId pathId;
        uint32_t sourceUplink = 0, localMetric = 0, remoteMetric = 0, score = 0;
        bool remoteValid = false;
    };
    // Mode, source leaf, destination leaf, path ID, and uplink (-1 for local).
    typedef std::tuple<std::string, uint32_t, uint32_t, uint32_t, int> SelectionKey;
    struct SelectionCount { uint64_t selected = 0, unknownRemote = 0; };
    std::map<uint32_t, Link> _links;
    std::map<uint32_t, int> _flowCores;
    std::vector<Route> _routes;
    std::vector<Decision> _decisions;
    std::map<SelectionKey, SelectionCount> _selections;
    uint64_t _routeViolations = 0;
    std::ofstream _linkFile, _routeFile, _summaryFile;
    std::ofstream _decisionFile, _selectionFile;
};

#endif
