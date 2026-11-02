#include "dre-queue-wrapper.h"

DreQueueWrapper::DreQueueWrapper(Queue &queue, linkspeed_bps rate,
                                 simtime_picosec interval,
                                 double alpha, int sourceUplink)
    : _queue(queue), _dre(rate, interval, alpha), _sourceUplink(sourceUplink),
      _downstreamLogger(queue.logger())
{
    // Every queue discipline reports PKT_SERVICE immediately before sendOn().
    // Interpose on that notification, retaining the original statistics logger.
    _queue.setLogger(this);
}

DreQueueWrapper::~DreQueueWrapper()
{
    _queue.setLogger(_downstreamLogger);
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

void DreQueueWrapper::logQueue(Queue &queue, QueueEvent event, Packet &packet)
{
    if (event == PKT_SERVICE) {
        const auto now = EventList::Get().now();
        _dre.recordPacket(packet.size(), now);
        packet.accumulateCe(_dre.metric(now));
    }
    if (_downstreamLogger != nullptr) {
        _downstreamLogger->logQueue(queue, event, packet);
    }
}
