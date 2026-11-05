// AI-generated: created by OpenAI Codex.
#ifndef EXPERIMENT_STATS_H
#define EXPERIMENT_STATS_H

#include "../loggertypes.h"
#include "../flow-generator.h"
#include "../congestion/dre-queue-wrapper.h"
#include "../load-balancers/route-decision.h"

#include <fstream>
#include <map>
#include <vector>
#include <tuple>
#include <memory>

struct RunConfiguration {
    std::string mode, workload, endhost, queue;
    double load = 0;
    uint32_t seed = 0, routingSeed = 0, flowsizeBytes = 0;
    double durationSeconds = 0;
    bool small = false, verbose = false;
    uint32_t cores = 0, leaves = 0, serversPerLeaf = 0;
    linkspeed_bps serverLinkBps = 0, fabricLinkBps = 0;
    double linkDelayUs = 0, dreIntervalUs = 0, dreAlpha = 0;
    uint32_t dreLevels = 8;
};

// Aggregate link counters and structured run/flow output; no per-packet disk writes.
class ExperimentStats : public QueueLogger
{
public:
    explicit ExperimentStats(const std::string &prefix);
    // Installs a bound logger; this recorder must outlive queue notifications.
    void registerLink(Queue &queue, linkspeed_bps speed, int core = -1,
                      DreQueueWrapper *dre = nullptr);
    void logQueue(Queue &queue, QueueEvent event, Packet &packet) override;
    void recordRoute(uint64_t flowId, uint32_t source, uint32_t destination, int core);
    void recordDecision(const RouteDecision &decision);
    void configureRun(const RunConfiguration &config);
    void recordCompletion(const FlowCompletion &flow);
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
    // Map nodes have stable addresses. Bind once instead of looking up the
    // queue ID for every enqueue, service, and drop notification.
    class LinkLogger : public QueueLogger {
    public:
        LinkLogger(ExperimentStats &owner, Link &link) : _owner(owner), _link(link) {}
        void logQueue(Queue &, QueueEvent event, Packet &packet) override {
            _owner.recordQueue(_link, event, packet);
        }
    private:
        ExperimentStats &_owner;
        Link &_link;
    };
    void recordQueue(Link &link, QueueEvent event, Packet &packet);
    struct Route { uint64_t flowId; uint32_t source, destination; int core; };
    // Mode, source leaf, destination leaf, path ID, and uplink (-1 for local).
    typedef std::tuple<std::string, uint32_t, uint32_t, uint32_t, int> SelectionKey;
    struct SelectionCount { uint64_t selected = 0, unknownRemote = 0; };
    std::map<uint32_t, Link> _links;
    std::vector<std::unique_ptr<LinkLogger>> _linkLoggers;
    std::map<uint32_t, int> _flowCores;
    std::vector<Route> _routes;
    std::vector<RouteDecision> _decisions;
    std::map<SelectionKey, SelectionCount> _selections;
    uint64_t _routeViolations = 0;
    uint64_t _completedRecords = 0;
    bool _configured = false;
    RunConfiguration _config;
    std::ofstream _linkFile, _routeFile, _summaryFile;
    std::ofstream _decisionFile, _selectionFile;
    std::ofstream _flowFile, _runFile;
};

#endif
