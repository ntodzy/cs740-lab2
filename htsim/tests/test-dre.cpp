#include "../congestion/dre-queue-wrapper.h"
#include "../packets/datapacket.h"
#include "../fairqueue.h"
#include "../aprx-fairqueue.h"
#include "../priorityqueue.h"
#include "../stoc-fairqueue.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void testTracker()
{
    // Capacity over tau = 8 Mbps * 1 ms / 8 = 1000 bytes.
    DreTracker dre(speedFromMbps(8), timeFromMs(1));
    check(dre.metric(0) == 0, "Empty estimator must read zero");
    dre.recordPacket(124, 0);
    check(dre.metric(0) == 0, "Below first quantization boundary");
    dre.recordPacket(1, 0);
    check(dre.metric(0) == 1, "Exact quantization boundary");
    dre.recordPacket(375, 0);
    check(dre.metric(0) == 4, "Same-time arrivals must accumulate without decay");
    const double afterTau = dre.normalizedLoad(timeFromMs(1));
    check(std::abs(afterTau - 0.5 / std::exp(1.0)) < 1e-12, "Incorrect exponential decay");
    check(dre.metric(timeFromMs(100)) == 0, "Metric reads must decay an idle link");
    dre.recordPacket(1000000, timeFromMs(100));
    check(dre.metric(timeFromMs(100)) == 7, "Metric must saturate at levels - 1");
    check(dre.offeredBytes() == 1000500, "Offered byte counter must not decay");
    bool rejected = false;
    try { dre.metric(0); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Backwards time must be rejected");
    for (int invalid = 0; invalid < 3; ++invalid) {
        rejected = false;
        try {
            DreTracker bad(invalid == 0 ? 0 : 8000000,
                           invalid == 1 ? 0 : timeFromMs(1), invalid == 2 ? 1 : 8);
        } catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "Invalid DRE configuration must be rejected");
    }
}

class TestPacket : public Packet
{
public:
    TestPacket(PacketFlow &flow, route_t &route, mem_b bytes, packetid_t id) {
        set(flow, route, bytes, id);
    }
    void free() override { freed = true; }
    bool freed = false;
};

class Sink : public PacketSink
{
public:
    void receivePacket(Packet &packet) override {
        ids.push_back(packet.id());
        ce.push_back(packet.congaMetadata().ce);
        tags.push_back(packet.congaMetadata().lbTag);
        packet.free();
    }
    std::vector<packetid_t> ids;
    std::vector<uint32_t> ce, tags;
};

class Counter : public QueueLogger
{
public:
    void logQueue(Queue &, QueueEvent event, Packet &) override {
        if (event == PKT_ENQUEUE) ++enqueued;
        if (event == PKT_SERVICE) ++serviced;
        if (event == PKT_DROP) ++dropped;
    }
    unsigned int enqueued = 0, serviced = 0, dropped = 0;
};

std::unique_ptr<Queue> makeQueue(unsigned int type, QueueLogger *logger)
{
    switch (type) {
    case 1: return std::unique_ptr<Queue>(new FairQueue(10000000000ULL, 10000, logger));
    case 2: return std::unique_ptr<Queue>(new AprxFairQueue(10000000000ULL, 10000, logger));
    case 3: return std::unique_ptr<Queue>(new PriorityQueue(10000000000ULL, 10000, logger));
    case 4: return std::unique_ptr<Queue>(new StocFairQueue(10000000000ULL, 10000, logger));
    default: return std::unique_ptr<Queue>(new Queue(10000000000ULL, 10000, logger));
    }
}

struct Result {
    simtime_picosec duration;
    std::vector<packetid_t> order;
};

Result runQueue(unsigned int type, bool wrapped)
{
    Counter counter;
    auto queue = makeQueue(type, &counter);
    DreQueueWrapper wrapper(*queue, 10000000000ULL, timeFromUs(160), 8, 3);
    Sink sink;
    route_t route{wrapped ? static_cast<PacketSink*>(&wrapper) : queue.get(), &sink};
    PacketFlow flow(nullptr);
    TestPacket first(flow, route, 1000, 1), second(flow, route, 500, 2);
    auto start = EventList::Get().now();
    first.sendOn();
    second.sendOn();
    while (EventList::Get().doNextEvent()) {}
    check(first.freed && second.freed, "Wrapped packets failed to reach sink");
    check(counter.enqueued == 2 && counter.serviced == 2 && counter.dropped == 0,
          "A packet was skipped or enqueued twice");
    if (wrapped) {
        check(wrapper.offeredBytes() == 1500, "Wrapper must count each arrival once");
        check(first.hasCongaMetadata() && second.hasCongaMetadata(), "Ingress must initialize metadata");
        check(sink.tags[0] == 3 && sink.tags[1] == 3, "Wrong uplink tag");
    }
    return Result{EventList::Get().now() - start, sink.ids};
}

void testPathAndDrop()
{
    // First-hop metric saturates; the next, faster link must not lower CE.
    Queue first(8000000, 10000, nullptr), second(800000000, 10000, nullptr);
    DreQueueWrapper ingress(first, 8000000, timeFromUs(1), 8, 5);
    DreQueueWrapper downstream(second, 800000000, timeFromMs(1));
    Sink sink;
    route_t route{&ingress, &downstream, &sink};
    PacketFlow flow(nullptr);
    TestPacket packet(flow, route, 1000, 1);
    packet.sendOn();
    const auto initial = packet.congaMetadata().ce;
    while (EventList::Get().doNextEvent()) {}
    check(initial == 7 && sink.ce[0] == initial && sink.tags[0] == 5,
          "CE decreased or tag changed along the fabric path");

    Queue full(8000000, 0, nullptr);
    DreQueueWrapper dropping(full, 8000000);
    route_t dropRoute{&dropping};
    TestPacket dropped(flow, dropRoute, 1000, 2);
    dropped.sendOn();
    check(dropped.freed && dropping.offeredBytes() == 1000,
          "Offered load must include packets dropped on arrival");
    check(!dropped.hasCongaMetadata(), "Downstream wrappers must not invent metadata");
}

void testPools()
{
    PacketFlow flow(nullptr);
    route_t route;
    auto *data = DataPacket::newpkt(flow, route, 1, 1000);
    data->initializeConga(4);
    data->accumulateCe(7);
    data->free();
    auto *reused = DataPacket::newpkt(flow, route, 1, 1000);
    check(reused == data && !reused->hasCongaMetadata() &&
          reused->congaMetadata().ce == 0 && reused->congaMetadata().lbTag == 0,
          "Pooled data packet leaked metadata");
    reused->free();
    auto *ack = DataAck::newpkt(flow, route, 1, 1000);
    ack->initializeConga(4);
    ack->accumulateCe(7);
    ack->free();
    auto *reusedAck = DataAck::newpkt(flow, route, 1, 1000);
    check(reusedAck == ack && !reusedAck->hasCongaMetadata() &&
          reusedAck->congaMetadata().ce == 0 && reusedAck->congaMetadata().lbTag == 0,
          "Pooled ACK leaked metadata");
    reusedAck->free();
}

int main()
{
    testTracker();
    for (unsigned int type = 0; type < 5; ++type) {
        auto baseline = runQueue(type, false);
        auto wrapped = runQueue(type, true);
        check(baseline.duration == wrapped.duration && baseline.order == wrapped.order,
              "DRE wrapper changed queue scheduling");
    }
    testPathAndDrop();
    testPools();
    std::cout << "DRE decay/quantization, all queue disciplines, CE, drops, and packet reuse passed\n";
}
