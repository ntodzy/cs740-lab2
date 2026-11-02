#ifndef DRE_H
#define DRE_H

#include "../htsim.h"

// Arrival-based continuous exponential approximation, using simulation time.
class DreTracker
{
public:
    DreTracker(linkspeed_bps rate, simtime_picosec timeConstant = timeFromUs(160),
               uint32_t levels = 8);
    void recordPacket(mem_b bytes, simtime_picosec now);
    double normalizedLoad(simtime_picosec now);
    uint32_t metric(simtime_picosec now);
    uint64_t offeredBytes() const { return _offeredBytes; }

private:
    void decay(simtime_picosec now);
    simtime_picosec _timeConstant;
    uint32_t _levels;
    simtime_picosec _lastUpdate = 0;
    double _bytes = 0;
    double _capacityBytes;
    uint64_t _offeredBytes = 0;
};

#endif // DRE_H
