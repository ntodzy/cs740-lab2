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
    // tau = 125 us / 0.125 = 1 ms; C*tau = 1000 bytes at 8 Mbps.
    DreTracker dre(speedFromMbps(8), timeFromUs(125), 0.125);
    check(dre.metric(0) == 0, "Empty estimator must read zero");
    dre.recordPacket(124, 0);
    check(dre.metric(0) == 0, "Below first quantization boundary");
    dre.recordPacket(1, 0);
    check(dre.metric(0) == 1, "Exact quantization boundary");
    dre.recordPacket(375, 0);
    check(dre.metric(0) == 4, "Same-time transmissions must accumulate without decay");
    check(dre.metric(timeFromUs(124)) == 4, "No decay is allowed before a Tdre tick");
    check(std::abs(dre.normalizedLoad(timeFromUs(125)) - 0.5 * 0.875) < 1e-12,
          "Exact tick must multiply X by 1-alpha");
    check(std::abs(dre.normalizedLoad(timeFromUs(200)) - 0.5 * 0.875) < 1e-12,
          "Mid-period reads must not apply partial decay");
    check(std::abs(dre.normalizedLoad(timeFromUs(250)) - 0.5 * 0.875 * 0.875) < 1e-12,
          "Reads must not shift the global tick boundaries");
    const double afterTau = dre.normalizedLoad(timeFromMs(1));
    check(std::abs(afterTau - 0.5 * std::pow(0.875, 8)) < 1e-12,
          "Missed ticks must apply the discrete rule, not the exponential approximation");
    check(dre.metric(timeFromMs(100)) == 0, "Metric reads must decay an idle link");
    dre.recordPacket(1000000, timeFromMs(100));
    check(dre.metric(timeFromMs(100)) == 7, "Metric must saturate at levels - 1");
    check(dre.sentBytes() == 1000500, "Transmitted byte counter must not decay");
    bool rejected = false;
    try { dre.metric(0); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Backwards time must be rejected");
    for (int invalid = 0; invalid < 4; ++invalid) {
        rejected = false;
        try {
            DreTracker bad(invalid == 0 ? 0 : 8000000,
                           invalid == 1 ? 0 : timeFromMs(1),
                           invalid == 2 ? 0 : (invalid == 3 ? 1 : 0.125));
        } catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "Invalid DRE configuration must be rejected");
    }

    DreTracker atTick(speedFromMbps(8), timeFromUs(125), 0.125);
    atTick.recordPacket(500, 0);
    atTick.recordPacket(125, timeFromUs(125));
    check(std::abs(atTick.normalizedLoad(timeFromUs(125)) - 0.5625) < 1e-12,
          "At a tick, decay must happen before adding transmitted bytes");
    DreTracker constantRate(speedFromMbps(8), timeFromUs(125), 0.125);
    for (unsigned int tick = 1; tick <= 200; ++tick) {
        constantRate.recordPacket(125, timeFromUs(125) * tick);
    }
    check(std::abs(constantRate.normalizedLoad(timeFromUs(125) * 200) - 1) < 1e-10,
          "Steady traffic must converge to X = R * Tdre / alpha");
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
    std::unique_ptr<DreQueueWrapper> wrapper;
    if (wrapped) {
        wrapper.reset(new DreQueueWrapper(*queue, 10000000000ULL, timeFromUs(20), 0.125, 3));
    }
    Sink sink;
    route_t route{wrapped ? static_cast<PacketSink*>(wrapper.get()) : queue.get(), &sink};
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(0, 1);
    TestPacket first(flow, route, 1000, 1), second(flow, route, 500, 2);
    auto start = EventList::Get().now();
    first.sendOn();
    second.sendOn();
    if (wrapped) check(wrapper->sentBytes() == 0, "Queued packets must not count as sent");
    while (EventList::Get().doNextEvent()) {}
    check(first.freed && second.freed, "Wrapped packets failed to reach sink");
    check(counter.enqueued == 2 && counter.serviced == 2 && counter.dropped == 0,
          "A packet was skipped or enqueued twice");
    if (wrapped) {
        check(wrapper->sentBytes() == 1500, "Wrapper must count each transmission once");
        check(first.hasCongaMetadata() && second.hasCongaMetadata(), "Ingress must initialize metadata");
        check(sink.tags[0] == 3 && sink.tags[1] == 3, "Wrong uplink tag");
        wrapper.reset();
        check(queue->logger() == &counter, "Wrapper must restore the original logger");
    }
    return Result{EventList::Get().now() - start, sink.ids};
}

void testPathAndDrop()
{
    // First-hop metric saturates; the next, faster link must not lower CE.
    Queue first(8000000, 10000, nullptr), second(800000000, 10000, nullptr);
    DreQueueWrapper ingress(first, 8000000, timeFromUs(1), 0.5, 5);
    DreQueueWrapper downstream(second, 800000000, timeFromUs(125), 0.125);
    Sink sink;
    route_t route{&ingress, &downstream, &sink};
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(0, 1);
    TestPacket packet(flow, route, 1000, 1);
    packet.sendOn();
    check(packet.congaMetadata().ce == 0 && ingress.sentBytes() == 0,
          "CE and DRE must not be updated until transmission");
    while (EventList::Get().doNextEvent()) {}
    check(sink.ce[0] == 7 && sink.tags[0] == 5,
          "CE decreased or tag changed along the fabric path");

    Queue full(8000000, 0, nullptr);
    DreQueueWrapper dropping(full, 8000000);
    route_t dropRoute{&dropping};
    TestPacket dropped(flow, dropRoute, 1000, 2);
    dropped.sendOn();
    check(dropped.freed && dropping.sentBytes() == 0,
          "Dropped arrivals must not count as transmitted load");
    check(!dropped.hasCongaMetadata(), "Downstream wrappers must not invent metadata");
}

void testPools()
{
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(0, 3);
    route_t route;
    auto *data = DataPacket::newpkt(flow, route, 1, 1000);
    check(data->congaMetadata().sourceLeaf == 0 && data->congaMetadata().destinationLeaf == 3,
          "Data leaf addressing is incorrect, including leaf zero");
    data->initializeConga(4);
    data->accumulateCe(7);
    data->setCongaFeedback(9, 2); // Must not be echoed back as the new observation.
    auto *ack = DataAck::newpkt(flow, route, 1, 1000, &data->congaMetadata());
    check(ack->hasCongaFeedback() && !ack->hasCongaMetadata() &&
          ack->congaMetadata().sourceLeaf == 3 && ack->congaMetadata().destinationLeaf == 0 &&
          ack->congaMetadata().feedbackLbTag == 4 && ack->congaMetadata().feedbackMetric == 7,
          "ACK must carry the forward observation with reversed leaf IDs");
    data->free();
    flow.setCongaLeafIds(5, 6);
    auto *reused = DataPacket::newpkt(flow, route, 1, 1000);
    check(reused == data && !reused->hasCongaMetadata() &&
          !reused->hasCongaFeedback() && reused->congaMetadata().ce == 0 &&
          reused->congaMetadata().lbTag == 0 && reused->congaMetadata().feedbackLbTag == 0 &&
          reused->congaMetadata().feedbackMetric == 0 &&
          reused->congaMetadata().sourceLeaf == 5 && reused->congaMetadata().destinationLeaf == 6,
          "Pooled data packet leaked metadata");
    reused->free();
    ack->initializeConga(2);
    ack->accumulateCe(3);
    check(ack->hasCongaMetadata() && ack->congaMetadata().lbTag == 2 &&
          ack->congaMetadata().ce == 3 && ack->hasCongaFeedback() &&
          ack->congaMetadata().feedbackLbTag == 4 && ack->congaMetadata().feedbackMetric == 7,
          "Reverse measurement initialization erased or aliased feedback");
    ack->free();
    auto *reusedAck = DataAck::newpkt(flow, route, 1, 1000);
    check(reusedAck == ack && !reusedAck->hasCongaMetadata() &&
          !reusedAck->hasCongaFeedback() && reusedAck->congaMetadata().ce == 0 &&
          reusedAck->congaMetadata().lbTag == 0 && reusedAck->congaMetadata().feedbackLbTag == 0 &&
          reusedAck->congaMetadata().feedbackMetric == 0 &&
          reusedAck->congaMetadata().sourceLeaf == 6 && reusedAck->congaMetadata().destinationLeaf == 5,
          "Pooled ACK leaked metadata");
    reusedAck->free();

    PacketFlow unconfigured(nullptr);
    auto *plain = DataPacket::newpkt(unconfigured, route, 1, 1000);
    plain->initializeConga(2);
    check(!plain->hasCongaMetadata() && !plain->congaMetadata().hasLeafIds(),
          "Missing leaf context must not become valid");
    auto *plainAck = DataAck::newpkt(unconfigured, route, 1, 1000, &plain->congaMetadata());
    check(!plainAck->hasCongaFeedback(), "Invalid observation must not produce valid feedback");
    plainAck->free();
    plain->free();

    CongaMetadata wrongFlow;
    wrongFlow.valid = true;
    wrongFlow.sourceLeaf = 10;
    wrongFlow.destinationLeaf = 11;
    auto *wrongAck = DataAck::newpkt(flow, route, 1, 1000, &wrongFlow);
    check(!wrongAck->hasCongaFeedback(), "Observation for another leaf pair must not become feedback");
    wrongAck->free();
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
