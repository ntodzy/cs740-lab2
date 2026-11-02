// AI-generated: created by OpenAI Codex.
#include "experiment-stats.h"

#include <iomanip>
#include <stdexcept>

ExperimentStats::ExperimentStats(const std::string &prefix)
    : _linkFile(prefix + ".links.csv"), _routeFile(prefix + ".routes.csv"),
      _summaryFile(prefix + ".summary.csv")
{
    if (!_linkFile || !_routeFile || !_summaryFile) {
        throw std::runtime_error("Cannot open experiment statistics: " + prefix);
    }
}

void ExperimentStats::registerLink(Queue &queue, linkspeed_bps speed, int core,
                                   DreQueueWrapper *dre)
{
    Link link;
    link.name = queue.str();
    link.speed = speed;
    link.core = core;
    link.dre = dre;
    _links.emplace(queue.id, link);
}

void ExperimentStats::logQueue(Queue &queue, QueueEvent event, Packet &packet)
{
    Link &link = _links.at(queue.id);
    if (event == PKT_ENQUEUE) {
        ++link.arrivals;
        link.arrivalBytes += packet.size();
    } else if (event == PKT_SERVICE) {
        ++link.departures;
        link.departureBytes += packet.size();
        if (link.core >= 0) {
            auto entry = _flowCores.emplace(packet.flow().id, link.core);
            if (!entry.second && entry.first->second != link.core) {
                ++_routeViolations;
            }
        }
    } else if (event == PKT_DROP) {
        ++link.drops;
        link.dropBytes += packet.size();
    }
}

void ExperimentStats::recordRoute(uint32_t source, uint32_t destination, int core)
{
    _routes.push_back(Route{source, destination, core});
}

void ExperimentStats::write(const FlowGenerator &generator, simtime_picosec endTime)
{
    _routeFile << "flow_id,source,destination,core\n";
    for (size_t i = 0; i < _routes.size(); ++i) {
        const Route &route = _routes[i];
        _routeFile << i << ',' << route.source << ',' << route.destination
                   << ',' << route.core << '\n';
    }
    _linkFile << "link,bitrate_bps,enqueued_packets,enqueued_bytes,departed_packets,"
                 "departed_bytes,dropped_packets,dropped_bytes,utilization,"
                 "dre_metric,dre_normalized_load,dre_sent_bytes\n";
    for (const auto &entry : _links) {
        const Link &link = entry.second;
        double utilization = endTime == 0 ? 0 :
            link.departureBytes * 8.0 / (link.speed * timeAsSec(endTime));
        _linkFile << link.name << ',' << link.speed << ',' << link.arrivals << ','
                  << link.arrivalBytes << ',' << link.departures << ','
                  << link.departureBytes << ',' << link.drops << ',' << link.dropBytes
                  << ',' << std::setprecision(17) << utilization;
        if (link.dre != nullptr) {
            _linkFile << ',' << link.dre->congestionMetric(endTime)
                      << ',' << link.dre->normalizedLoad(endTime)
                      << ',' << link.dre->sentBytes();
        } else {
            _linkFile << ",,,"; // Host links are not DRE monitored.
        }
        _linkFile << '\n';
    }
    _summaryFile << "generated,completed,incomplete,route_violations\n"
                 << generator.generatedFlows() << ','
                 << generator.generatedFlows() - generator.liveFlows() << ','
                 << generator.liveFlows() << ',' << _routeViolations << '\n';
    _linkFile.flush();
    _routeFile.flush();
    _summaryFile.flush();
    if (!_linkFile || !_routeFile || !_summaryFile) {
        throw std::runtime_error("Failed to write experiment statistics");
    }
}
