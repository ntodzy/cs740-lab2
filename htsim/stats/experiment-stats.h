// AI-generated: created by OpenAI Codex.
#ifndef EXPERIMENT_STATS_H
#define EXPERIMENT_STATS_H

#include "../loggertypes.h"
#include "../flow-generator.h"
#include "../congestion/dre-queue-wrapper.h"

#include <fstream>
#include <map>
#include <vector>

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
    std::map<uint32_t, Link> _links;
    std::map<uint32_t, int> _flowCores;
    std::vector<Route> _routes;
    uint64_t _routeViolations = 0;
    std::ofstream _linkFile, _routeFile, _summaryFile;
};

#endif
