#ifndef DRE_QUEUE_WRAPPER_H
#define DRE_QUEUE_WRAPPER_H

#include "../queue.h"
#include "dre.h"

// Non-owning decorator: the queue must outlive this wrapper and all events.
// Put the wrapper, not the wrapped queue, into each monitored route hop.
class DreQueueWrapper : public PacketSink, public QueueLogger
{
public:
    DreQueueWrapper(Queue &queue, linkspeed_bps rate,
                    simtime_picosec interval = timeFromUs(20),
                    double alpha = 0.125, int sourceUplink = -1);
    ~DreQueueWrapper() override;
    DreQueueWrapper(const DreQueueWrapper &) = delete;
    DreQueueWrapper &operator=(const DreQueueWrapper &) = delete;
    void receivePacket(Packet &packet) override;
    void logQueue(Queue &queue, QueueEvent event, Packet &packet) override;
    uint32_t congestionMetric(simtime_picosec now) { return _dre.metric(now); }
    uint32_t congestionMetric() { return congestionMetric(EventList::Get().now()); }
    double normalizedLoad(simtime_picosec now) { return _dre.normalizedLoad(now); }
    uint64_t sentBytes() const { return _dre.sentBytes(); }

private:
    Queue &_queue;
    DreTracker _dre;
    int _sourceUplink;
    QueueLogger *_downstreamLogger;
};

#endif // DRE_QUEUE_WRAPPER_H
