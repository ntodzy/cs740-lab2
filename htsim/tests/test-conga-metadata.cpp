#include "../tcp.h"
#include "../timely.h"
#include "../packetpair.h"
#include "../congestion/dre-queue-wrapper.h"

#include <stdexcept>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

class AckReceiver : public DataSource
{
public:
    AckReceiver() : DataSource(nullptr, 1500, 0) {
        _flow.setCongaLeafIds(0, 2);
    }
    void printStatus() override {}
    void doNextEvent() override {}
    void receivePacket(Packet &packet) override {
        observed = packet.congaMetadata();
        ecn = packet.getFlag(Packet::ECN_REV);
        ++received;
        packet.free();
    }
    CongaMetadata observed;
    uint32_t received = 0;
    bool ecn = false;
};

template<class Sink>
void testSink(bool reverseFabric, bool echoesEcn = true)
{
    AckReceiver source;
    Sink sink;
    Queue queue(10000000000ULL, 10000, nullptr);
    DreQueueWrapper ingress(queue, 10000000000ULL, timeFromUs(20), 0.125, 8);
    route_t reverse;
    if (reverseFabric) reverse.push_back(&ingress);
    reverse.push_back(&source);
    sink.connect(source, reverse);
    route_t unused;
    auto *packet = DataPacket::newpkt(source._flow, unused, 1, 1500);
    packet->set_ts(EventList::Get().now());
    packet->setFlag(Packet::ECN_FWD);
    packet->initializeConga(4);
    packet->accumulateCe(6);
    packet->setCongaFeedback(11, 1); // Unrelated reverse observation on the data.
    sink.receivePacket(*packet);
    while (EventList::Get().doNextEvent()) {}
    const auto &metadata = source.observed;
    require(source.received == 1, "Sink did not generate exactly one ACK");
    require(metadata.sourceLeaf == 2 && metadata.destinationLeaf == 0,
            "ACK leaf IDs were not reversed");
    require(metadata.feedbackValid && metadata.feedbackLbTag == 4 && metadata.feedbackMetric == 6,
            "ACK lost the incoming observation or echoed the wrong feedback");
    require(metadata.valid == reverseFabric, "Unexpected reverse measurement validity");
    if (reverseFabric) {
        require(metadata.lbTag == 8 && metadata.ce == 0,
                "Reverse fabric did not start an independent measurement");
    }
    require(source._flow._nPackets == 0, "Sink leaked data or ACK packets");
    require(source.ecn == echoesEcn, "Metadata handling changed existing ECN echo behavior");
}

int main()
{
    for (bool reverseFabric : {false, true}) {
        testSink<TcpSink>(reverseFabric);
        testSink<TimelySink>(reverseFabric, false);
        testSink<PacketPairSink>(reverseFabric);
    }
    std::cout << "TCP/DCTCP, TIMELY, and packet-pair ACK metadata survived copying and reverse ingress\n";
}
