#ifndef DRE_QUEUE_WRAPPER_H
#define DRE_QUEUE_WRAPPER_H

#include "../queue.h"
#include "dre.h"

// Non-owning decorator: the queue must outlive this wrapper and all events.
// Put the wrapper, not the wrapped queue, into each monitored route hop.
class DreQueueWrapper : public PacketSink
{
public:
    DreQueueWrapper(Queue &queue, linkspeed_bps rate,
                    simtime_picosec timeConstant = timeFromUs(160),
                    uint32_t levels = 8, int sourceUplink = -1);
    void receivePacket(Packet &packet) override;
    uint32_t congestionMetric(simtime_picosec now) { return _dre.metric(now); }
    uint32_t congestionMetric() { return congestionMetric(EventList::Get().now()); }
    double normalizedLoad(simtime_picosec now) { return _dre.normalizedLoad(now); }
    uint64_t offeredBytes() const { return _dre.offeredBytes(); }

private:
    Queue &_queue;
    DreTracker _dre;
    int _sourceUplink;
};

#endif // DRE_QUEUE_WRAPPER_H
