#include "dre.h"

#include <stdexcept>

DreTracker::DreTracker(linkspeed_bps rate, simtime_picosec interval, double alpha)
    : _interval(interval), _alpha(alpha), _capacityBytes(0)
{
    if (rate == 0 || interval == 0 || !(alpha > 0 && alpha < 1)) {
        throw std::invalid_argument("DRE requires positive rate/Tdre and 0 < alpha < 1");
    }
    // tau = Tdre / alpha; convert C from bits/s to bytes/s for byte-valued X.
    _capacityBytes = (rate / 8.0) * (timeAsSec(interval) / alpha);
}

void DreTracker::decay(simtime_picosec now)
{
    if (now < _lastUpdate) {
        throw std::invalid_argument("DRE timestamps cannot move backwards");
    }
    // Previous continuous approximation (retained for reference):
    // _bytes *= std::exp(-double(now - _lastUpdate) / double(_timeConstant));
    // Apply only whole periodic ticks. Reading mid-period must not shift them.
    const uint64_t ticks = (now - _lastDecayTick) / _interval;
    if (ticks != 0) {
        _bytes *= std::pow(1.0 - _alpha, double(ticks));
        _lastDecayTick += ticks * _interval;
    }
    _lastUpdate = now;
}

void DreTracker::recordPacket(mem_b bytes, simtime_picosec now)
{
    decay(now);
    _bytes += bytes;
    _sentBytes += bytes;
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
        return 7;
    }
    return static_cast<uint32_t>(load * 8); // Quantize C*tau-normalized load to 3 bits.
}
