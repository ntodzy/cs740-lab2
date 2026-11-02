#include "dre.h"

#include <stdexcept>

DreTracker::DreTracker(linkspeed_bps rate, simtime_picosec timeConstant,
                       uint32_t levels)
    : _timeConstant(timeConstant), _levels(levels),
      _capacityBytes(rate * timeAsSec(timeConstant) / 8.0)
{
    if (rate == 0 || timeConstant == 0 || levels < 2) {
        throw std::invalid_argument("DRE requires positive rate/time constant and at least two levels");
    }
}

void DreTracker::decay(simtime_picosec now)
{
    if (now < _lastUpdate) {
        throw std::invalid_argument("DRE timestamps cannot move backwards");
    }
    _bytes *= std::exp(-double(now - _lastUpdate) / double(_timeConstant));
    _lastUpdate = now;
}

void DreTracker::recordPacket(mem_b bytes, simtime_picosec now)
{
    decay(now);
    _bytes += bytes;
    _offeredBytes += bytes;
}

double DreTracker::normalizedLoad(simtime_picosec now)
{
    decay(now); // Reads must account for idle time, too.
    return _bytes / _capacityBytes;
}

uint32_t DreTracker::metric(simtime_picosec now)
{
    double load = normalizedLoad(now);
    if (load >= 1.0) {
        return _levels - 1;
    }
    return static_cast<uint32_t>(load * _levels);
}
