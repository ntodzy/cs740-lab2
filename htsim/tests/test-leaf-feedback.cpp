#include "../leaf-switches/leafswitch.h"
#include "../congestion/dre-queue-wrapper.h"
#include "../tcp.h"
#include "../pipe.h"

#include <stdexcept>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

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

// Inject a controlled fabric observation at the leaf's incoming hook.
void observe(LeafSwitch &leaf, uint32_t source, uint32_t uplink, uint32_t metric,
             bool feedback = false, uint32_t feedbackUplink = 0, uint32_t feedbackMetric = 0)
{
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(source, leaf.leafId());
    Receiver receiver;
    route_t route{&leaf, &receiver};
    auto *packet = DataPacket::newpkt(flow, route, 1, 1500);
    packet->initializeConga(uplink);
    packet->accumulateCe(metric);
    if (feedback) packet->setCongaFeedback(feedbackUplink, feedbackMetric);
    packet->sendOn();
    require(receiver.received == 1 && flow._nPackets == 0, "Incoming hook lost a packet");
}

CongaMetadata outgoing(LeafSwitch &leaf, uint32_t destination)
{
    PacketFlow flow(nullptr);
    flow.setCongaLeafIds(leaf.leafId(), destination);
    Receiver receiver;
    route_t route{&leaf.egress(), &receiver};
    auto *packet = DataPacket::newpkt(flow, route, 1, 1500);
    packet->setCongaFeedback(2, 7); // Endpoint echo must be replaced or cleared.
    packet->sendOn();
    require(receiver.received == 1 && flow._nPackets == 0, "Outgoing hook lost a packet");
    return receiver.observed;
}

class Advance : public EventSource
{
public:
    Advance() : EventSource("advance") {}
    void doNextEvent() override {}
};

void testSchedulingAndExpiry()
{
    LeafSwitch leaf(1, 3, 3, timeFromUs(100));
    const auto start = EventList::Get().now();
    require(!leaf.toLeaf(0, 0, start).valid && !leaf.fromLeaf(0, 0, start).valid,
            "Tables must start unknown");
    for (uint32_t u = 0; u < 3; ++u) observe(leaf, 0, u, u);
    require(leaf.fromLeaf(0, 0, start).valid && leaf.fromLeaf(0, 0, start).metric == 0,
            "Measured zero must be distinguishable from unknown");
    for (uint32_t u : {0, 1, 2, 0}) {
        const auto feedback = outgoing(leaf, 0);
        require(feedback.feedbackValid && feedback.feedbackLbTag == u && feedback.feedbackMetric == u,
                "New entries and unchanged refreshes must use round-robin");
    }
    observe(leaf, 0, 2, 6);
    observe(leaf, 0, 2, 6); // Repeated observation must preserve pending priority.
    auto feedback = outgoing(leaf, 0);
    require(feedback.feedbackLbTag == 2 && feedback.feedbackMetric == 6,
            "Changed metric must precede unchanged entries");
    require(!leaf.fromLeaf(0, 2, start).pending, "Sent entry retained pending priority");
    require(outgoing(leaf, 0).feedbackLbTag == 0, "Round-robin cursor did not advance");
    require(!outgoing(leaf, 2).feedbackValid, "Feedback leaked into another peer's traffic");
    require(!outgoing(leaf, 1).feedbackValid, "Same-leaf traffic carried fabric feedback");

    // Both portions of the header are processed, with independent uplink tags.
    observe(leaf, 2, 1, 4, true, 2, 7);
    require(leaf.fromLeaf(2, 1, start).metric == 4 && leaf.toLeaf(2, 2, start).metric == 7,
            "Incoming observation and feedback updated the wrong cells");
    require(!leaf.toLeaf(2, 1, start).valid && !leaf.toLeaf(0, 2, start).valid,
            "Feedback overwrote unrelated state");
    observe(leaf, 2, 3, 7, true, 3, 7); // Invalid tags must be ignored safely.
    observe(leaf, 2, 0, 8, true, 0, 8); // Reject non-three-bit metrics.
    observe(leaf, 3, 0, 7); // Invalid peer.
    require(!leaf.fromLeaf(2, 0, start).valid && !leaf.toLeaf(2, 0, start).valid,
            "Invalid metadata created entries");
    // A packet sent to the wrong incoming hook must not modify its tables.
    {
        PacketFlow flow(nullptr);
        flow.setCongaLeafIds(2, 0);
        Receiver receiver;
        route_t route{&leaf, &receiver};
        auto *packet = DataPacket::newpkt(flow, route, 1, 1500);
        packet->initializeConga(0);
        packet->accumulateCe(7);
        packet->setCongaFeedback(0, 7);
        packet->sendOn();
        require(receiver.received == 1 && !leaf.fromLeaf(2, 0, start).valid &&
                !leaf.toLeaf(2, 0, start).valid, "Wrong-destination packet modified state");
    }

    Advance timer;
    EventList::Get().sourceIsPending(timer, start + timeFromUs(100) - 1);
    require(EventList::Get().doNextEvent(), "Expiry test did not advance time");
    require(leaf.fromLeaf(0, 0, EventList::Get().now()).valid, "Entry expired too early");
    require(outgoing(leaf, 0).feedbackValid, "Fresh feedback was unavailable before expiry");
    EventList::Get().sourceIsPending(timer, start + timeFromUs(100));
    require(EventList::Get().doNextEvent(), "Expiry boundary did not advance time");
    require(!leaf.fromLeaf(0, 0, EventList::Get().now()).valid &&
            !leaf.toLeaf(2, 2, EventList::Get().now()).valid,
            "From/To entries must expire at the exact boundary");
    require(!outgoing(leaf, 0).feedbackValid, "Expired state or endpoint echo was sent");
    observe(leaf, 0, 2, 6); // Same value after expiry is new feedback.
    require(leaf.fromLeaf(0, 2, EventList::Get().now()).pending,
            "Fresh observation did not reactivate expired entry");
    require(outgoing(leaf, 0).feedbackLbTag == 2, "Reactivated entry was not sent");
    EventList::Get().sourceIsPending(timer, start + timeFromUs(150));
    require(EventList::Get().doNextEvent(), "Refresh test did not advance time");
    observe(leaf, 0, 2, 6);
    require(!leaf.fromLeaf(0, 2, EventList::Get().now()).pending,
            "Unchanged observation incorrectly received changed priority");
    EventList::Get().sourceIsPending(timer, start + timeFromUs(200));
    require(EventList::Get().doNextEvent(), "Refresh boundary did not advance time");
    require(leaf.fromLeaf(0, 2, EventList::Get().now()).valid,
            "Repeated observation did not refresh its timestamp");
}

class AckReceiver : public DataSource
{
public:
    AckReceiver() : DataSource(nullptr, 1500, 0) { _flow.setCongaLeafIds(0, 1); }
    void printStatus() override {}
    void doNextEvent() override {}
    void receivePacket(Packet &packet) override {
        observed = packet.congaMetadata();
        ++received;
        packet.free();
    }
    CongaMetadata observed;
    uint32_t received = 0;
};

void testFabricRoundTrip()
{
    LeafSwitch a(0, 3, 3), b(1, 3, 3);
    AckReceiver source;
    TcpSink sink;
    Queue forwardQueue(8000000000ULL, 10000, nullptr);
    Queue reverseQueue(8000000000ULL, 10000, nullptr);
    DreQueueWrapper forward(forwardQueue, 8000000000ULL, timeFromUs(1), 0.5, 1);
    DreQueueWrapper reverse(reverseQueue, 8000000000ULL, timeFromUs(1), 0.5, 2);
    Pipe forwardPipe(timeFromUs(0.1)), reversePipe(timeFromUs(0.1));
    route_t dataRoute{&a.egress(), &forward, &forwardPipe, &b, &sink};
    route_t ackRoute{&b.egress(), &reverse, &reversePipe, &a, &source};
    sink.connect(source, ackRoute);
    auto *packet = DataPacket::newpkt(source._flow, dataRoute, 1, 1500);
    packet->set_ts(EventList::Get().now());
    packet->sendOn();
    while (EventList::Get().doNextEvent()) {}
    const auto now = EventList::Get().now();
    require(source.received == 1 && source._flow._nPackets == 0, "Data/ACK round trip failed");
    require(b.fromLeaf(0, 1, now).valid && b.fromLeaf(0, 1, now).metric == 6,
            "A->B did not record B.FromLeaf[A][1]");
    require(a.toLeaf(1, 1, now).valid && a.toLeaf(1, 1, now).metric == 6,
            "B->A feedback did not update A.ToLeaf[B][1]");
    require(a.fromLeaf(1, 2, now).valid && a.fromLeaf(1, 2, now).metric == 0,
            "ACK did not independently measure its own uplink 2");
    require(source.observed.lbTag == 2 && source.observed.feedbackLbTag == 1,
            "Return uplink replaced original uplink in feedback");
    for (uint32_t peer = 0; peer < 3; ++peer) {
        for (uint32_t u = 0; u < 3; ++u) {
            require(b.fromLeaf(peer, u, now).valid == (peer == 0 && u == 1),
                    "B.FromLeaf changed an unrelated cell");
            require(a.toLeaf(peer, u, now).valid == (peer == 1 && u == 1),
                    "A.ToLeaf changed an unrelated cell");
        }
    }
    // A later DATA packet also carries feedback about the reverse ACK path.
    const auto feedback = outgoing(a, 1);
    require(feedback.feedbackValid && feedback.feedbackLbTag == 2 && feedback.feedbackMetric == 0,
            "Feedback was restricted to ACKs");
}

int main()
{
    testSchedulingAndExpiry();
    testFabricRoundTrip();
    std::cout << "Leaf tables, changed-first round-robin, expiry, and real TCP/DRE feedback round trip passed\n";
}
