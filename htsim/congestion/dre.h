#ifndef DRE_H
#define DRE_H

#include "../htsim.h"

// Periodic DRE: add transmitted bytes; at each Tdre tick, X *= (1 - alpha).
// Ticks are anchored at simulation time zero and evaluated lazily on access.
class DreTracker
{
public:
    DreTracker(linkspeed_bps rate, simtime_picosec interval = timeFromUs(20),
               double alpha = 0.125);
    void recordPacket(mem_b bytes, simtime_picosec now);
    double normalizedLoad(simtime_picosec now);
    uint32_t metric(simtime_picosec now);
    uint64_t sentBytes() const { return _sentBytes; }

private:
    void decay(simtime_picosec now);
    simtime_picosec _interval;
    double _alpha;
    simtime_picosec _lastDecayTick = 0;
    simtime_picosec _lastUpdate = 0;
    double _bytes = 0;
    double _capacityBytes;
    uint64_t _sentBytes = 0;
};

#endif // DRE_H
