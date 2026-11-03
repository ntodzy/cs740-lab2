#include "../load-balancers/ecmp.h"
#include "../load-balancers/conga.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <type_traits>

static_assert(!std::is_convertible<uint32_t, PathId>::value,
              "Candidate indices must not implicitly become path IDs");
static_assert(!std::is_convertible<PathId, uint32_t>::value,
              "Path IDs must not implicitly become core numbers");

class RouteHop : public PacketSink
{
public:
    void receivePacket(Packet &) override {
        throw std::runtime_error("Route construction must not transmit packets");
    }
};

template<class Policy>
void checkRouteAbstraction()
{
    RouteHop hops[8];
    // IDs deliberately differ from candidate indices; each direction may have
    // a different hop count. Neither option describes a specific switch tier.
    const std::vector<RoutePath> paths{
        {PathId(101), {&hops[0]}, {&hops[1], &hops[2], &hops[3]}},
        {PathId(0), {&hops[1], &hops[2], &hops[3]}, {&hops[0]}},
        {PathId(9001), {&hops[3], &hops[2]}, {&hops[1], &hops[0]}}
    };
    RouteContext context(paths);
    context.srcHost = 12;
    context.dstHost = 29;
    context.srcLeaf = 1;
    context.dstLeaf = 2;
    context.usesFabric = true;
    context.forward.access = {&hops[4]};
    context.forward.delivery = {&hops[5]};
    context.reverse.access = {&hops[6]};
    context.reverse.delivery = {&hops[7]};
    Policy policy(42);
    bool sawZeroId = false;
    for (uint64_t flow = 0; flow < 100; ++flow) {
        context.flowId = flow;
        RoutePath route = policy.selectRoute(context);
        const auto candidate = std::find_if(paths.begin(), paths.end(), [&](const RoutePath &path) {
            return path.pathId == route.pathId;
        });
        if (candidate == paths.end()) throw std::runtime_error("Policy returned an unknown path ID");
        const auto &expected = *candidate;
        route_t expectedForward = context.forward.access;
        expectedForward.insert(expectedForward.end(), expected.forward.begin(), expected.forward.end());
        expectedForward.insert(expectedForward.end(), context.forward.delivery.begin(), context.forward.delivery.end());
        route_t expectedReverse = context.reverse.access;
        expectedReverse.insert(expectedReverse.end(), expected.reverse.begin(), expected.reverse.end());
        expectedReverse.insert(expectedReverse.end(), context.reverse.delivery.begin(), context.reverse.delivery.end());
        if (route.pathId != expected.pathId ||
            route.forward != expectedForward || route.reverse != expectedReverse) {
            throw std::runtime_error("Opaque path IDs or variable-length route ordering failed");
        }
        sawZeroId |= route.pathId == PathId(0);
        // Callers own the returned hop vectors; consuming them must not mutate
        // cached candidates or affect the next selection.
        route.forward.clear();
        route.reverse.clear();
        const auto again = policy.selectRoute(context);
        if (again.forward != expectedForward || again.reverse != expectedReverse) {
            throw std::runtime_error("Returned route modified cached topology candidates");
        }
    }
    if (!sawZeroId) throw std::runtime_error("Path ID zero was not exercised");

    // The topology's flag determines whether a fabric path is needed.
    context.usesFabric = false;
    context.directPathId = PathId(77);
    const auto local = policy.selectRoute(context);
    if (local.pathId != PathId(77) || local.forward != route_t{&hops[4], &hops[5]} ||
        local.reverse != route_t{&hops[6], &hops[7]}) {
        throw std::runtime_error("Local routes incorrectly included a fabric path");
    }

    const std::vector<RoutePath> noPaths;
    RouteContext missing(noPaths);
    missing.usesFabric = true;
    bool rejected = false;
    try {
        policy.selectRoute(missing);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("Missing fabric candidates were not rejected");
    }

    // The same policy instance must accept a different number of candidates:
    // every request's context, rather than constructor state, defines its choices.
    const std::vector<RoutePath> singlePath{paths[2]};
    RouteContext single(singlePath);
    single.usesFabric = true;
    const auto only = policy.selectRoute(single);
    if (only.pathId != singlePath[0].pathId || only.forward != singlePath[0].forward ||
        only.reverse != singlePath[0].reverse) {
        throw std::runtime_error("Policy did not derive its choices from the new context");
    }
}

int main()
{
    std::srand(12314);
    const int expectedTrafficDraw = std::rand();
    std::srand(12314);

    EcmpLoadBalancer first(42);
    EcmpLoadBalancer repeat(42);
    EcmpLoadBalancer different(43);
    CongaLoadBalancer scaffold(42);
    std::vector<RoutePath> paths(12);
    for (uint32_t index = 0; index < paths.size(); ++index) {
        paths[index].pathId = PathId(index);
    }
    auto selectId = [&](LoadBalancer &policy, uint32_t source, uint32_t destination,
                        uint64_t flowId) {
        RouteContext context(paths);
        context.srcHost = source;
        context.dstHost = destination;
        context.flowId = flowId;
        context.usesFabric = true;
        return policy.selectRoute(context).pathId;
    };
    bool differs = false;
    unsigned int counts[12] = {};
    for (unsigned int i = 0; i < 120000; i++) {
        const PathId choice = selectId(first, i % 768, (i + 37) % 768, i);
        // Unrelated calls must not affect a flow's core mapping.
        selectId(repeat, 500, 600, i + 900000);
        if (choice.value >= 12 || choice != selectId(repeat, i % 768, (i + 37) % 768, i)) {
            std::cerr << "Invalid or nonrepeatable uplink choice\n";
            return 1;
        }
        ++counts[choice.value];
        if (choice != selectId(scaffold, i % 768, (i + 37) % 768, i)) {
            std::cerr << "CONGA scaffold differs from its ECMP placeholder\n";
            return 1;
        }
        differs |= choice != selectId(different, i % 768, (i + 37) % 768, i);
    }
    if (!differs || std::rand() != expectedTrafficDraw) {
        std::cerr << "Routing seeds are ineffective or routing consumed traffic randomness\n";
        return 1;
    }
    for (unsigned int core = 0; core < 12; ++core) {
        // A generous deterministic regression bound around 10,000 per core.
        if (counts[core] < 9200 || counts[core] > 10800) {
            std::cerr << "Unbalanced ECMP distribution on core " << core << '\n';
            return 1;
        }
        std::cout << "Core " << core << ": " << counts[core] << " flows\n";
    }
    std::cout << "Hash stability, 12-core distribution, and traffic RNG isolation passed\n";
    checkRouteAbstraction<EcmpLoadBalancer>();
    checkRouteAbstraction<CongaLoadBalancer>();
    std::cout << "Opaque IDs, variable-length paths, local routes, and missing-candidate checks passed\n";
}
