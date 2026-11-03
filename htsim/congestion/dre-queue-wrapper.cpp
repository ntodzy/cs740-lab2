#include "dre-queue-wrapper.h"
#include <stdexcept>

DreQueueWrapper::DreQueueWrapper(Queue &queue, linkspeed_bps rate,
                                 simtime_picosec interval,
                                 double alpha, int sourceUplink)
    : _queue(queue), _dre(rate, interval, alpha), _sourceUplink(sourceUplink)
{
    if (_queue.serviceHook() != nullptr) {
        throw std::invalid_argument("Queue already has a packet service hook");
    }
    _queue.setServiceHook(this);
}

DreQueueWrapper::~DreQueueWrapper()
{
    if (_queue.serviceHook() == this) _queue.setServiceHook(nullptr);
}

void DreQueueWrapper::receivePacket(Packet &packet)
{
    // Leaf uplinks begin a new directional fabric traversal. ACKs get their
    // own fresh CE while retaining feedback about the data's forward path.
    if (_sourceUplink >= 0) {
        packet.initializeConga(static_cast<uint32_t>(_sourceUplink));
    }
    // Previous arrival-based measurement (retained for reference):
    // _dre.recordPacket(packet.size(), EventList::Get().now());
    // packet.accumulateCe(_dre.metric(EventList::Get().now()));
    _queue.receivePacket(packet);
    // Do not touch packet here: the queue may have dropped and freed it.
}

void DreQueueWrapper::onPacketService(Queue &, Packet &packet)
{
    const auto now = EventList::Get().now();
    _dre.recordPacket(packet.size(), now);
    packet.accumulateCe(_dre.metric(now));
}
