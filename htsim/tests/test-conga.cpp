#include "../load-balancers/conga.h"
#include "../load-balancers/ecmp.h"
#include "../leaf-switches/leafswitch.h"
#include "../congestion/dre-queue-wrapper.h"
#include "../packets/datapacket.h"
#include "../tcp.h"
#include "../pipe.h"
#include "../stats/experiment-stats.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <string>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

// Test setup resolves opaque IDs once; selection consumes aligned snapshots.
CandidateCongestion &measurement(RouteContext &context, PathId id)
{
    if (context.congestion.size() < context.paths.size()) {
        context.congestion.resize(context.paths.size());
    }
    for (size_t i = 0; i < context.paths.size(); ++i) {
        if (context.paths[i].pathId == id) return context.congestion[i];
    }
    throw std::runtime_error("Unknown test path ID");
}

class Hop : public PacketSink
{
public:
    void receivePacket(Packet &) override {
        throw std::runtime_error("Selection must not transmit packets");
    }
};

class Receiver : public PacketSink
{
public:
    void receivePacket(Packet &packet) override {
        observed = packet.congaMetadata();
        ++received;
        packet.free();
    }
    CongaMetadata observed;
    uint32_t received = 0;
};

// Populate source-leaf ToLeaf state through the actual packet receive hook.
void feedback(LeafSwitch &source, uint32_t remote, uint32_t uplink, uint32_t metric)
{
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(remote, source.leafId());
    Receiver receiver;
    route_t route{&source, &receiver};
    auto *packet = DataPacket::newpkt(flow, route, 1, 1500);
    packet->setCongaFeedback(uplink, metric);
    packet->sendOn();
    require(receiver.received == 1 && flow._nPackets == 0, "Feedback injection lost a packet");
}

void configure(RouteContext &context, const LeafSwitch &source)
{
    context.srcHost = 12;
    context.dstHost = 29;
    context.srcLeaf = source.leafId();
    context.dstLeaf = 1;
    context.usesFabric = true;
    context.now = EventList::Get().now();
}

void refreshFeedback(RouteContext &context, const LeafSwitch &source)
{
    for (auto &entry : context.congestion) {
        auto &measurement = entry;
        const auto remote = source.toLeaf(context.dstLeaf, measurement.sourceUplink, context.now);
        measurement.remoteMetric = remote.metric;
        measurement.remoteValid = remote.valid;
    }
}

void expectPath(CongaLoadBalancer &policy, const RouteContext &context, PathId expected,
                const char *message)
{
    const auto route = policy.selectRoute(context);
    require(route.pathId == expected, message);
    const auto candidate = std::find_if(context.paths.begin(), context.paths.end(),
        [&](const RoutePath &path) { return path.pathId == expected; });
    require(candidate != context.paths.end(), "Expected test candidate is missing");
    route_t forward = context.forward.access;
    forward.insert(forward.end(), candidate->forward.begin(), candidate->forward.end());
    forward.insert(forward.end(), context.forward.delivery.begin(), context.forward.delivery.end());
    route_t reverse = context.reverse.access;
    reverse.insert(reverse.end(), candidate->reverse.begin(), candidate->reverse.end());
    reverse.insert(reverse.end(), context.reverse.delivery.begin(), context.reverse.delivery.end());
    require(route.forward == forward && route.reverse == reverse,
            "Selection did not preserve the chosen candidate's ordered hops");
}

void testLocalRemoteAndStale()
{
    LeafSwitch source(0, 3, 3, timeFromUs(100));
    Hop hops[8];
    const std::vector<RoutePath> paths{
        {PathId(101), {&hops[0]}, {&hops[1], &hops[2], &hops[3]}},
        {PathId(0), {&hops[1], &hops[2], &hops[3]}, {&hops[0]}},
        {PathId(9001), {&hops[3], &hops[2]}, {&hops[1], &hops[0]}}
    };
    RouteContext context(paths);
    configure(context, source);
    context.forward.access = {&hops[4]};
    context.forward.delivery = {&hops[5]};
    context.reverse.access = {&hops[6]};
    context.reverse.delivery = {&hops[7]};
    // Candidate order, IDs, and source-uplink columns are deliberately unrelated.
    measurement(context, PathId(101)) = CandidateCongestion{2, 1};
    measurement(context, PathId(0)) = CandidateCongestion{0, 5};
    measurement(context, PathId(9001)) = CandidateCongestion{1, 7};
    CongaLoadBalancer policy(42);
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(101), "Local congestion did not choose the minimum");
    auto reordered = paths;
    std::reverse(reordered.begin(), reordered.end());
    RouteContext permuted(reordered);
    configure(permuted, source);
    permuted.congestion = context.congestion;
    std::reverse(permuted.congestion.begin(), permuted.congestion.end());
    const auto reorderedRoute = policy.selectRoute(permuted);
    require(reorderedRoute.pathId == PathId(101) && reorderedRoute.metrics.sourceUplink == 2 &&
            reorderedRoute.metrics.localMetric == 1,
            "Reordered snapshots lost their candidate binding");

    feedback(source, 1, 2, 6);
    // Table changes are invisible until a new context snapshot is constructed.
    expectPath(policy, context, PathId(101), "Selection reached live leaf state");
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(0), "Remote congestion did not change the selected path");
    feedback(source, 1, 0, 2);
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(0), "Score did not use max(local, remote)");
    // A third leaf reporting the same uplink must not affect destination 1.
    feedback(source, 2, 0, 7);
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(0), "Selection read the wrong destination's table");

    context.now = timeFromUs(100) - 1;
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(0), "Remote feedback expired before its boundary");
    context.now = timeFromUs(100);
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(101), "Expired feedback did not revert to local-only scoring");
    require(!source.toLeaf(1, 2, context.now).valid, "Expiry test did not exercise stale state");

    // Returned route vectors belong to the caller, independent of cached hops.
    auto owned = policy.selectRoute(context);
    owned.forward.clear();
    owned.reverse.clear();
    refreshFeedback(context, source);
    expectPath(policy, context, PathId(101), "Consuming a returned route damaged cached paths");

    // One policy can select among a different number of candidates on its next call.
    const std::vector<RoutePath> one{paths[2]};
    RouteContext single(one);
    configure(single, source);
    measurement(single, PathId(9001)) = CandidateCongestion{1, 0};
    expectPath(policy, single, PathId(9001), "Policy cached the previous candidate count");

    const std::vector<RoutePath> none;
    RouteContext local(none);
    local.directPathId = PathId(77);
    local.forward.access = {&hops[4]};
    local.forward.delivery = {&hops[5]};
    local.reverse.access = {&hops[6]};
    local.reverse.delivery = {&hops[7]};
    const auto direct = policy.selectRoute(local);
    require(direct.pathId == PathId(77) && direct.forward == route_t{&hops[4], &hops[5]} &&
            direct.reverse == route_t{&hops[6], &hops[7]},
            "Direct routing incorrectly required congestion state");
}

void testTiesAndRandomness()
{
    LeafSwitch source(0, 2, 12);
    std::vector<RoutePath> paths(12);
    for (uint32_t i = 0; i < paths.size(); ++i) paths[i].pathId = PathId(101 + 37 * i);
    auto reordered = paths;
    std::reverse(reordered.begin(), reordered.end());
    RouteContext context(paths), permuted(reordered);
    configure(context, source);
    configure(permuted, source);
    for (uint32_t i = 0; i < paths.size(); ++i) {
        measurement(context, paths[i].pathId) = CandidateCongestion{i, 0};
    }
    permuted.congestion = context.congestion;
    std::reverse(permuted.congestion.begin(), permuted.congestion.end());
    CongaLoadBalancer first(42), repeat(42), different(43);
    std::srand(12314);
    const int expectedDraw = std::rand();
    std::srand(12314);
    uint32_t counts[12] = {};
    bool differs = false;
    for (uint64_t flow = 0; flow < 12000; ++flow) {
        context.flowId = flow;
        permuted.flowId = flow;
        const PathId selected = first.selectRoute(context).pathId;
        require(selected == repeat.selectRoute(context).pathId,
                "Ties are not reproducible for the same seed");
        require(selected == first.selectRoute(permuted).pathId,
                "Candidate order changed a tied selection");
        differs |= selected != different.selectRoute(context).pathId;
        const auto found = std::find_if(paths.begin(), paths.end(),
            [&](const RoutePath &path) { return path.pathId == selected; });
        require(found != paths.end(), "Tie-break returned an unknown path ID");
        ++counts[std::distance(paths.begin(), found)];
    }
    require(differs, "Routing seed has no effect on tied CONGA choices");
    require(std::rand() == expectedDraw, "CONGA consumed the traffic RNG stream");
    for (uint32_t count : counts) {
        require(count >= 800 && count <= 1200, "Ties did not spread traffic across all 12 paths");
    }
}

template<class Action>
void rejects(Action action, const char *message)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, message);
}

void testInvalidContext()
{
    LeafSwitch source(0, 2, 2);
    const std::vector<RoutePath> paths{{PathId(101), {}, {}}, {PathId(9001), {}, {}}};
    RouteContext context(paths);
    configure(context, source);
    measurement(context, PathId(101)) = CandidateCongestion{0, 0};
    measurement(context, PathId(9001)) = CandidateCongestion{1, 0};
    CongaLoadBalancer policy(42);
    const auto saved = context.congestion;
    context.congestion.clear();
    rejects([&] { policy.selectRoute(context); }, "Empty snapshot vector was accepted");
    context.congestion = saved;
    measurement(context, PathId(101)).available = false;
    rejects([&] { policy.selectRoute(context); }, "Missing candidate metadata was accepted");
    measurement(context, PathId(101)) = CandidateCongestion{0, 8};
    rejects([&] { policy.selectRoute(context); }, "Non-three-bit local metric was accepted");
    measurement(context, PathId(101)) = CandidateCongestion{16, 0};
    rejects([&] { policy.selectRoute(context); }, "Non-four-bit uplink tag was accepted");
    measurement(context, PathId(101)) = CandidateCongestion{0, 0, 8, true};
    rejects([&] { policy.selectRoute(context); }, "Non-three-bit remote metric was accepted");
    measurement(context, PathId(101)) = CandidateCongestion{1, 0};
    rejects([&] { policy.selectRoute(context); }, "Duplicate source uplinks were accepted");
    measurement(context, PathId(101)) = CandidateCongestion{0, 0};
    const std::vector<RoutePath> duplicates{paths[0], paths[0]};
    RouteContext duplicate(duplicates);
    configure(duplicate, source);
    duplicate.congestion = context.congestion;
    rejects([&] { policy.selectRoute(duplicate); }, "Duplicate opaque path IDs were accepted");
    const std::vector<RoutePath> none;
    RouteContext missing(none);
    configure(missing, source);
    rejects([&] { policy.selectRoute(missing); }, "Empty fabric candidates were accepted");
    std::vector<RoutePath> maximumPaths;
    for (uint32_t i = 0; i < 16; ++i) maximumPaths.emplace_back(PathId(100 + i));
    RouteContext maximum(maximumPaths);
    configure(maximum, source);
    for (uint32_t i = 0; i < 16; ++i) maximum.congestion.emplace_back(i, 0);
    policy.selectRoute(maximum); // Exercise all slots in the bounded validation array.
    maximumPaths.emplace_back(PathId(116));
    maximum.congestion.emplace_back(0, 0);
    rejects([&] { policy.selectRoute(maximum); }, "More than 16 unique uplinks were accepted");
}

class AckReceiver : public DataSource
{
public:
    AckReceiver() : DataSource(nullptr, 1500, 0) { _flow.setCongaLeafIds(0, 1); }
    void printStatus() override {}
    void doNextEvent() override {}
    void receivePacket(Packet &packet) override {
        ++received;
        packet.free();
    }
    uint32_t received = 0;
};

void testDecisionDiagnostics(const std::string &prefix)
{
    LeafSwitch source(0, 2, 2, timeFromUs(100));
    const std::vector<RoutePath> paths{{PathId(800), {}, {}}, {PathId(20), {}, {}}};
    RouteContext context(paths);
    configure(context, source);
    measurement(context, PathId(800)) = CandidateCongestion{1, 1};
    measurement(context, PathId(20)) = CandidateCongestion{0, 3};
    CongaLoadBalancer policy(42);
    {
        ExperimentStats stats(prefix);
        auto record = [&] {
            refreshFeedback(context, source);
            const auto route = policy.selectRoute(context);
            const RouteDecision decision("conga", context, route);
            const auto saved = context.congestion;
            context.congestion.clear(); // Recording cannot depend on the routing context.
            stats.recordDecision(decision);
            context.congestion = saved;
            ++context.flowId;
        };
        record(); // Unknown path 800: score 1 wins over path 20's local score 3.
        feedback(source, 1, 1, 6);
        record(); // Known path 800 now scores 6; unknown path 20 wins with 3.
        feedback(source, 1, 0, 2);
        record(); // Path 20 remains best: max(local=3, remote=2) is 3.
        context.now = timeFromUs(100);
        record(); // Both entries expire; local-only score 1 wins again.
        FlowGenerator generator(DataSource::DCTCP, route_gen_t{}, 1000000000ULL,
                                10000, Workloads::UNIFORM);
        stats.write(generator, context.now);
    }
    std::ifstream decisions(prefix + ".decisions.csv");
    std::string line;
    require(bool(std::getline(decisions, line)), "Decision CSV header is missing");
    const std::vector<std::string> expected{
        "0,0,conga,12,29,0,1,1,800,1,1,0,0,1",
        "1,0,conga,12,29,0,1,1,20,0,3,0,0,3",
        "2,0,conga,12,29,0,1,1,20,0,3,2,1,3",
        "3,100,conga,12,29,0,1,1,800,1,1,0,0,1"
    };
    for (const auto &row : expected) {
        require(bool(std::getline(decisions, line)) && line == row,
                "Chosen-path diagnostics disagree with controlled congestion inputs");
    }
    require(!std::getline(decisions, line), "Decision CSV contains unexpected rows");
    std::ifstream selections(prefix + ".selections.csv");
    require(bool(std::getline(selections, line)), "Selection CSV header is missing");
    for (const auto &row : {"conga,0,1,20,0,2,1", "conga,0,1,800,1,2,2"}) {
        require(bool(std::getline(selections, line)) && line == row,
                "Selection/unknown counts disagree with controlled decisions");
    }
    require(!std::getline(selections, line), "Selection CSV contains unexpected rows");
}

void testFeedbackChangesFutureFlow()
{
    LeafSwitch sourceLeaf(0, 2, 2), destinationLeaf(1, 2, 2);
    AckReceiver source;
    TcpSink sink;
    Queue firstQueue(8000000000ULL, 10000, nullptr);
    Queue secondQueue(8000000000ULL, 10000, nullptr);
    Queue ackQueue(8000000000ULL, 10000, nullptr);
    DreQueueWrapper firstFabric(firstQueue, 8000000000ULL, timeFromUs(1), 0.5, 0);
    DreQueueWrapper secondFabric(secondQueue, 8000000000ULL, timeFromUs(1), 0.5, 1);
    DreQueueWrapper reverseFabric(ackQueue, 8000000000ULL, timeFromUs(1), 0.5, 0);
    Pipe firstPipe(timeFromUs(0.1)), secondPipe(timeFromUs(0.1)), ackPipe(timeFromUs(0.1));
    const route_t reverse{&destinationLeaf.egress(), &reverseFabric, &ackPipe, &sourceLeaf};
    const std::vector<RoutePath> paths{
        {PathId(901), {&sourceLeaf.egress(), &firstFabric, &firstPipe, &destinationLeaf}, reverse},
        {PathId(103), {&sourceLeaf.egress(), &secondFabric, &secondPipe, &destinationLeaf}, reverse}
    };
    RouteContext context(paths);
    configure(context, sourceLeaf);
    context.forward.delivery = {&sink};
    context.reverse.delivery = {&source};
    measurement(context, PathId(901)) = CandidateCongestion{0, 0};
    measurement(context, PathId(103)) = CandidateCongestion{1, 1};
    CongaLoadBalancer policy(42);
    RoutePath installed = policy.selectRoute(context);
    require(installed.pathId == PathId(901), "Controlled flow did not start on uplink 0");
    const auto installedForward = installed.forward;
    const auto installedReverse = installed.reverse;
    sink.connect(source, installed.reverse);
    auto *data = DataPacket::newpkt(source._flow, installed.forward, 1, 1500);
    data->set_ts(EventList::Get().now());
    data->sendOn();
    while (EventList::Get().doNextEvent()) {}
    const auto now = EventList::Get().now();
    require(source.received == 1 && source._flow._nPackets == 0, "Real TCP feedback round trip failed");
    require(sourceLeaf.toLeaf(1, 0, now).valid && sourceLeaf.toLeaf(1, 0, now).metric == 6,
            "Measured path congestion did not reach the source table");
    context.now = now;
    refreshFeedback(context, sourceLeaf);
    ++context.flowId;
    expectPath(policy, context, PathId(103), "Returned congestion did not redirect a later flow");
    require(installed.forward == installedForward && installed.reverse == installedReverse,
            "A later flow decision rewrote an installed route");

    // A later packet from the original flow still follows its installed uplink.
    data = DataPacket::newpkt(source._flow, installed.forward, 1501, 1500);
    data->set_ts(EventList::Get().now());
    data->sendOn();
    while (EventList::Get().doNextEvent()) {}
    require(firstFabric.sentBytes() == 3000 && secondFabric.sentBytes() == 0,
            "An existing flow switched uplinks after feedback");
    require(source.received == 2 && source._flow._nPackets == 0,
            "The original flow did not complete its second packet/ACK round trip");

    // Send a new flow on the route chosen after feedback, not just its ID.
    AckReceiver laterSource;
    TcpSink laterSink;
    context.forward.delivery = {&laterSink};
    context.reverse.delivery = {&laterSource};
    context.now = EventList::Get().now();
    refreshFeedback(context, sourceLeaf);
    RoutePath later = policy.selectRoute(context);
    require(later.pathId == PathId(103), "New flow did not avoid the reported congested uplink");
    laterSink.connect(laterSource, later.reverse);
    data = DataPacket::newpkt(laterSource._flow, later.forward, 1, 1500);
    data->set_ts(EventList::Get().now());
    data->sendOn();
    while (EventList::Get().doNextEvent()) {}
    require(firstFabric.sentBytes() == 3000 && secondFabric.sentBytes() == 1500,
            "Selected route ID changed but packets did not use the alternate uplink");
    require(laterSource.received == 1 && laterSource._flow._nPackets == 0,
            "New flow did not complete on the feedback-selected route");
}

void testMetricsFreeRecording(const std::string &prefix)
{
    const std::vector<RoutePath> paths{{PathId(77), {}, {}}};
    RouteContext context(paths);
    context.flowId = 44;
    context.usesFabric = true;
    EcmpLoadBalancer policy(42);
    auto path = policy.selectRoute(context);
    require(!path.metrics.available, "ECMP unexpectedly required congestion snapshots");
    const RouteDecision decision("ecmp", context, path);
    context.srcHost = 999; // Subsequent context changes must not alter the record.
    ExperimentStats stats(prefix);
    stats.recordDecision(decision);
    FlowGenerator generator(DataSource::DCTCP, route_gen_t{}, 1000000000ULL,
                            10000, Workloads::UNIFORM);
    stats.write(generator, 0);
    std::ifstream decisions(prefix + ".decisions.csv");
    std::string line;
    std::getline(decisions, line);
    require(bool(std::getline(decisions, line)) && line == "44,0,ecmp,0,0,0,0,1,77,,,,,",
            "Recording required congestion state or reread changed routing data");
}

void testBoundQueueCounters(const std::string &prefix)
{
    Queue first(1000000000ULL, 1000000, nullptr);
    Queue second(1000000000ULL, 1000000, nullptr);
    first.setName("first");
    second.setName("second");
    ExperimentStats stats(prefix);
    stats.registerLink(first, 1000000000ULL, 3);
    stats.registerLink(second, 1000000000ULL, 4);
    rejects([&] { stats.registerLink(first, 1000000000ULL); },
            "Duplicate queue registration was accepted");
    PacketFlow flow(nullptr);
    route_t route;
    auto *packet = DataPacket::newpkt(flow, route, 1, 1500);
    for (int i = 0; i < 2; ++i) {
        first.logger()->logQueue(first, QueueLogger::PKT_ENQUEUE, *packet);
        first.logger()->logQueue(first, QueueLogger::PKT_SERVICE, *packet);
    }
    second.logger()->logQueue(second, QueueLogger::PKT_SERVICE, *packet);
    second.logger()->logQueue(second, QueueLogger::PKT_DROP, *packet);
    packet->free();
    FlowGenerator generator(DataSource::DCTCP, route_gen_t{}, 1000000000ULL,
                            10000, Workloads::UNIFORM);
    stats.write(generator, timeFromUs(100));
    std::ifstream links(prefix + ".links.csv"), summary(prefix + ".summary.csv");
    std::string line;
    bool foundFirst = false, foundSecond = false;
    while (std::getline(links, line)) {
        foundFirst |= line.find("first,1000000000,2,3000,2,3000,0,0,") == 0;
        foundSecond |= line.find("second,1000000000,0,0,1,1500,1,1500,") == 0;
    }
    std::getline(summary, line);
    require(foundFirst && foundSecond && bool(std::getline(summary, line)) && line == "0,0,0,1",
            "Bound queue counters mixed links or lost a route violation");
}

void testGeneratorOwnedIdentity(const std::string &prefix)
{
    Queue dataQueue(1000000000ULL, 1000000, nullptr);
    Queue ackQueue(1000000000ULL, 1000000, nullptr);
    ExperimentStats stats(prefix);
    stats.recordRoute(999, 0, 1, -1); // Existing observation history cannot allocate IDs.
    std::vector<uint64_t> ids;
    route_gen_t generate = [&](uint64_t flowId, route_t *&fwd, route_t *&rev,
                               uint32_t &src, uint32_t &dst) {
        ids.push_back(flowId);
        src = 0;
        dst = 1;
        fwd = new route_t{&dataQueue};
        rev = new route_t{&ackQueue};
        // Sample observations: omitted records must not change future identities.
        if (flowId % 2) stats.recordRoute(flowId, src, dst, -1);
    };
    FlowGenerator generator(DataSource::DCTCP, generate, 1000000000ULL,
                            10000, Workloads::UNIFORM);
    const auto now = EventList::Get().now();
    EventList::Get().setEndtime(now + timeFromUs(2000));
    generator.setTimeLimits(now + 1, now + timeFromUs(2000));
    while (EventList::Get().doNextEvent()) {}
    require(ids.size() > 3 && generator.generatedFlows() == ids.size(),
            "Identity regression did not generate enough flows");
    for (size_t i = 0; i < ids.size(); ++i) {
        require(ids[i] == i, "Recording history influenced flow identity");
    }
    stats.write(generator, EventList::Get().now());
    std::ifstream routes(prefix + ".routes.csv");
    std::string line;
    std::getline(routes, line);
    require(bool(std::getline(routes, line)) && line == "999,0,1,-1",
            "Route recording replaced supplied identity with a row index");
    for (size_t i = 1; i < ids.size(); i += 2) {
        require(bool(std::getline(routes, line)) && line == std::to_string(i) + ",0,1,-1",
                "Sampled route observations lost generator-assigned flow IDs");
    }
}

int main(int argc, char **argv)
{
    require(argc == 2, "Pass an artifact prefix for diagnostic CSV verification");
    testLocalRemoteAndStale();
    testTiesAndRandomness();
    testInvalidContext();
    testDecisionDiagnostics(argv[1]);
    testFeedbackChangesFutureFlow();
    testMetricsFreeRecording(std::string(argv[1]) + "-ecmp");
    testBoundQueueCounters(std::string(argv[1]) + "-counters");
    testGeneratorOwnedIdentity(std::string(argv[1]) + "-identity");
    std::cout << "CONGA scoring, unknown/stale policy, path mapping, tie-breaking, validation, and TCP feedback adaptation passed\n";
}
