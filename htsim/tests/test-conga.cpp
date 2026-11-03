#include "../load-balancers/conga.h"
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
    context.sourceLeafState = &source;
    context.usesFabric = true;
    context.now = EventList::Get().now();
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
    context.congestion[PathId(101)] = CandidateCongestion{2, 1};
    context.congestion[PathId(0)] = CandidateCongestion{0, 5};
    context.congestion[PathId(9001)] = CandidateCongestion{1, 7};
    CongaLoadBalancer policy(42);
    expectPath(policy, context, PathId(101), "Local congestion did not choose the minimum");

    feedback(source, 1, 2, 6);
    expectPath(policy, context, PathId(0), "Remote congestion did not change the selected path");
    feedback(source, 1, 0, 2);
    expectPath(policy, context, PathId(0), "Score did not use max(local, remote)");
    // A third leaf reporting the same uplink must not affect destination 1.
    feedback(source, 2, 0, 7);
    expectPath(policy, context, PathId(0), "Selection read the wrong destination's table");

    context.now = timeFromUs(100) - 1;
    expectPath(policy, context, PathId(0), "Remote feedback expired before its boundary");
    context.now = timeFromUs(100);
    expectPath(policy, context, PathId(101), "Expired feedback did not revert to local-only scoring");
    require(!source.toLeaf(1, 2, context.now).valid, "Expiry test did not exercise stale state");

    // Returned route vectors belong to the caller, independent of cached hops.
    auto owned = policy.selectRoute(context);
    owned.forward.clear();
    owned.reverse.clear();
    expectPath(policy, context, PathId(101), "Consuming a returned route damaged cached paths");

    // One policy can select among a different number of candidates on its next call.
    const std::vector<RoutePath> one{paths[2]};
    RouteContext single(one);
    configure(single, source);
    single.congestion[PathId(9001)] = CandidateCongestion{1, 0};
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
        context.congestion[paths[i].pathId] = CandidateCongestion{i, 0};
    }
    permuted.congestion = context.congestion;
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
    LeafSwitch source(0, 2, 2), wrongSource(1, 2, 2);
    const std::vector<RoutePath> paths{{PathId(101), {}, {}}, {PathId(9001), {}, {}}};
    RouteContext context(paths);
    configure(context, source);
    context.congestion[PathId(101)] = CandidateCongestion{0, 0};
    context.congestion[PathId(9001)] = CandidateCongestion{1, 0};
    CongaLoadBalancer policy(42);
    context.sourceLeafState = nullptr;
    rejects([&] { policy.selectRoute(context); }, "Missing source state was accepted");
    context.sourceLeafState = &wrongSource;
    rejects([&] { policy.selectRoute(context); }, "Wrong source leaf was accepted");
    context.sourceLeafState = &source;
    context.congestion.erase(PathId(101));
    rejects([&] { policy.selectRoute(context); }, "Missing candidate metadata was accepted");
    context.congestion[PathId(101)] = CandidateCongestion{0, 8};
    rejects([&] { policy.selectRoute(context); }, "Non-three-bit local metric was accepted");
    context.congestion[PathId(101)] = CandidateCongestion{16, 0};
    rejects([&] { policy.selectRoute(context); }, "Non-four-bit uplink tag was accepted");
    context.congestion[PathId(101)] = CandidateCongestion{2, 0};
    rejects([&] { policy.selectRoute(context); }, "Out-of-range source table column was accepted");
    context.congestion[PathId(101)] = CandidateCongestion{1, 0};
    rejects([&] { policy.selectRoute(context); }, "Duplicate source uplinks were accepted");
    context.congestion[PathId(101)] = CandidateCongestion{0, 0};
    context.dstLeaf = 2;
    rejects([&] { policy.selectRoute(context); }, "Out-of-range destination was accepted");
    context.dstLeaf = 1;
    const std::vector<RoutePath> duplicates{paths[0], paths[0]};
    RouteContext duplicate(duplicates);
    configure(duplicate, source);
    duplicate.congestion = context.congestion;
    rejects([&] { policy.selectRoute(duplicate); }, "Duplicate opaque path IDs were accepted");
    const std::vector<RoutePath> none;
    RouteContext missing(none);
    configure(missing, source);
    rejects([&] { policy.selectRoute(missing); }, "Empty fabric candidates were accepted");
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
    context.congestion[PathId(800)] = CandidateCongestion{1, 1};
    context.congestion[PathId(20)] = CandidateCongestion{0, 3};
    CongaLoadBalancer policy(42);
    {
        ExperimentStats stats(prefix);
        auto record = [&] {
            const auto route = policy.selectRoute(context);
            stats.recordDecision("conga", context, route);
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
    context.congestion[PathId(901)] = CandidateCongestion{0, 0};
    context.congestion[PathId(103)] = CandidateCongestion{1, 1};
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
}

int main(int argc, char **argv)
{
    require(argc == 2, "Pass an artifact prefix for diagnostic CSV verification");
    testLocalRemoteAndStale();
    testTiesAndRandomness();
    testInvalidContext();
    testDecisionDiagnostics(argv[1]);
    testFeedbackChangesFutureFlow();
    std::cout << "CONGA scoring, unknown/stale policy, path mapping, tie-breaking, validation, and TCP feedback adaptation passed\n";
}
