// AI-generated: created by OpenAI Codex.
#include "experiment-stats.h"

#include <iomanip>
#include <stdexcept>

ExperimentStats::ExperimentStats(const std::string &prefix)
    : _linkFile(prefix + ".links.csv"), _routeFile(prefix + ".routes.csv"),
      _summaryFile(prefix + ".summary.csv"), _decisionFile(prefix + ".decisions.csv"),
      _selectionFile(prefix + ".selections.csv"), _flowFile(prefix + ".flows.csv"),
      _runFile(prefix + ".run.json")
{
    if (!_linkFile || !_routeFile || !_summaryFile || !_decisionFile || !_selectionFile ||
        !_flowFile || !_runFile) {
        throw std::runtime_error("Cannot open experiment statistics: " + prefix);
    }
    _flowFile << "mode,workload,load,seed,routing_seed,flow_id,source,destination,"
                 "size_bytes,start_us,finish_us,fct_us\n" << std::setprecision(17);
}

void ExperimentStats::configureRun(const RunConfiguration &config)
{
    if (_configured) throw std::logic_error("Run configuration already set");
    _config = config;
    _configured = true;
}

void ExperimentStats::recordCompletion(const FlowCompletion &flow)
{
    if (!_configured || flow.finish < flow.start) {
        throw std::logic_error("Flow completion requires run configuration and valid timestamps");
    }
    _flowFile << _config.mode << ',' << _config.workload << ',' << _config.load << ','
              << _config.seed << ',' << _config.routingSeed << ',' << flow.flowId << ','
              << flow.source << ',' << flow.destination << ',' << flow.sizeBytes << ','
              << timeAsUs(flow.start) << ',' << timeAsUs(flow.finish) << ','
              << timeAsUs(flow.finish - flow.start) << '\n';
    ++_completedRecords;
}

void ExperimentStats::registerLink(Queue &queue, linkspeed_bps speed, int core,
                                   DreQueueWrapper *dre)
{
    Link link;
    link.name = queue.str();
    link.speed = speed;
    link.core = core;
    link.dre = dre;
    const auto entry = _links.emplace(queue.id, link);
    if (!entry.second) {
        throw std::invalid_argument("Queue already registered for experiment statistics");
    }
    _linkLoggers.emplace_back(new LinkLogger(*this, entry.first->second));
    queue.setLogger(_linkLoggers.back().get());
}

void ExperimentStats::logQueue(Queue &queue, QueueEvent event, Packet &packet)
{
    Link &link = _links.at(queue.id);
    recordQueue(link, event, packet);
}

void ExperimentStats::recordQueue(Link &link, QueueEvent event, Packet &packet)
{
    if (event == PKT_ENQUEUE) {
        ++link.arrivals;
        link.arrivalBytes += packet.size();
    } else if (event == PKT_SERVICE) {
        ++link.departures;
        link.departureBytes += packet.size();
        if (link.core >= 0) {
            const auto entry = _flowCores.find(packet.flow().id);
            if (entry == _flowCores.end()) {
                _flowCores.emplace(packet.flow().id, link.core);
            } else if (entry->second != link.core) {
                ++_routeViolations;
            }
        }
    } else if (event == PKT_DROP) {
        ++link.drops;
        link.dropBytes += packet.size();
    }
}

void ExperimentStats::recordRoute(uint64_t flowId, uint32_t source, uint32_t destination, int core)
{
    _routes.push_back(Route{flowId, source, destination, core});
}

void ExperimentStats::recordDecision(const RouteDecision &decision)
{
    const bool measured = decision.usesFabric && decision.metrics.available;
    const int uplink = measured ? static_cast<int>(decision.metrics.sourceUplink) : -1;
    SelectionCount &counts = _selections[SelectionKey(decision.mode, decision.sourceLeaf,
        decision.destinationLeaf, decision.pathId.value, uplink)];
    ++counts.selected;
    if (measured && !decision.metrics.remoteValid) ++counts.unknownRemote;
    _decisions.push_back(decision);
}

void ExperimentStats::write(const FlowGenerator &generator, simtime_picosec endTime)
{
    const uint64_t completed = generator.generatedFlows() - generator.liveFlows();
    if (_configured) {
        if (_completedRecords != completed) {
            throw std::logic_error("Completed-flow records disagree with generator counts");
        }
        // Configured strings are validated enum-like CLI values, not free text.
        _runFile << std::setprecision(17)
            << "{\n  \"schema_version\": 1,\n  \"config\": {\n"
            << "    \"experiment\": " << _config.experiment << ",\n    \"mode\": \"" << _config.mode
            << "\",\n    \"workload\": \"" << _config.workload
            << "\",\n    \"load\": " << _config.load
            << ",\n    \"seed\": " << _config.seed
            << ",\n    \"routing_seed\": " << _config.routingSeed
            << ",\n    \"duration_seconds\": " << _config.durationSeconds
            << ",\n    \"flowsize_bytes\": " << _config.flowsizeBytes
            << ",\n    \"endhost\": \"" << _config.endhost
            << "\",\n    \"queue\": \"" << _config.queue
            << "\",\n    \"small\": " << (_config.small ? "true" : "false")
            << ",\n    \"verbose\": " << (_config.verbose ? "true" : "false")
            << ",\n    \"cores\": " << _config.cores
            << ",\n    \"leaves\": " << _config.leaves
            << ",\n    \"servers_per_leaf\": " << _config.serversPerLeaf
            << ",\n    \"server_link_bps\": " << _config.serverLinkBps
            << ",\n    \"fabric_link_bps\": " << _config.fabricLinkBps
            << ",\n    \"link_delay_us\": " << _config.linkDelayUs
            << ",\n    \"dre_interval_us\": " << _config.dreIntervalUs
            << ",\n    \"dre_alpha\": " << _config.dreAlpha
            << ",\n    \"dre_levels\": " << _config.dreLevels
            << "\n  },\n  \"counts\": {\n"
            << "    \"generated\": " << generator.generatedFlows()
            << ",\n    \"completed\": " << completed
            << ",\n    \"incomplete\": " << generator.liveFlows()
            << ",\n    \"route_violations\": " << _routeViolations
            << "\n  },\n  \"end_time_us\": " << timeAsUs(endTime)
            << ",\n  \"events_processed\": " << EventList::Get()._nEventsProcessed << "\n}\n";
    }
    _decisionFile << "flow_id,time_us,mode,source,destination,source_leaf,destination_leaf,"
                     "uses_fabric,path_id,source_uplink,local_metric,remote_metric,remote_valid,score\n";
    for (const auto &decision : _decisions) {
        _decisionFile << decision.flowId << ',' << std::setprecision(17) << timeAsUs(decision.time)
                      << ',' << decision.mode << ',' << decision.source << ',' << decision.destination
                      << ',' << decision.sourceLeaf << ',' << decision.destinationLeaf
                      << ',' << decision.usesFabric << ',' << decision.pathId.value;
        if (decision.usesFabric && decision.metrics.available) {
            _decisionFile << ',' << decision.metrics.sourceUplink << ',' << decision.metrics.localMetric
                          << ',' << decision.metrics.remoteMetric << ',' << decision.metrics.remoteValid
                          << ',' << decision.metrics.score;
        } else {
            _decisionFile << ",,,,,"; // Direct route or no supplied measurements.
        }
        _decisionFile << '\n';
    }
    _selectionFile << "mode,source_leaf,destination_leaf,path_id,source_uplink,"
                      "selected_flows,unknown_remote_flows\n";
    for (const auto &entry : _selections) {
        const auto &key = entry.first;
        _selectionFile << std::get<0>(key) << ',' << std::get<1>(key) << ','
                       << std::get<2>(key) << ',' << std::get<3>(key) << ',';
        if (std::get<4>(key) >= 0) _selectionFile << std::get<4>(key);
        _selectionFile << ',' << entry.second.selected << ',' << entry.second.unknownRemote << '\n';
    }
    _routeFile << "flow_id,source,destination,core\n";
    for (size_t i = 0; i < _routes.size(); ++i) {
        const Route &route = _routes[i];
        _routeFile << route.flowId << ',' << route.source << ',' << route.destination
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
    _decisionFile.flush();
    _selectionFile.flush();
    _flowFile.flush();
    _runFile.flush();
    if (!_linkFile || !_routeFile || !_summaryFile || !_decisionFile || !_selectionFile ||
        !_flowFile || !_runFile) {
        throw std::runtime_error("Failed to write experiment statistics");
    }
}
