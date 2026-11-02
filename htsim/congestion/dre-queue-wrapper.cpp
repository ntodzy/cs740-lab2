#include "dre-queue-wrapper.h"

DreQueueWrapper::DreQueueWrapper(Queue &queue, linkspeed_bps rate,
                                 simtime_picosec timeConstant,
                                 uint32_t levels, int sourceUplink)
    : _queue(queue), _dre(rate, timeConstant, levels), _sourceUplink(sourceUplink)
{}

void DreQueueWrapper::receivePacket(Packet &packet)
{
    const auto now = EventList::Get().now();
    // Leaf uplinks begin a new directional fabric traversal. ACKs get their
    // own fresh CE; no reverse-feedback protocol is implemented here.
    if (_sourceUplink >= 0) {
        packet.initializeConga(static_cast<uint32_t>(_sourceUplink));
    }
    // Measure offered bytes, including arrivals that the queue later drops.
    _dre.recordPacket(packet.size(), now);
    packet.accumulateCe(_dre.metric(now));
    _queue.receivePacket(packet);
    // Do not touch packet here: the queue may have dropped and freed it.
}
