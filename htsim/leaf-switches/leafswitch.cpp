#include "leafswitch.h"
#include "../eventlist.h"

#include <stdexcept>

LeafSwitch::LeafSwitch(uint32_t leafId, uint32_t leafCount, uint32_t uplinkCount,
                       simtime_picosec expiry)
    : _leafId(leafId), _uplinkCount(uplinkCount), _expiry(expiry),
      _toLeaf(leafCount, std::vector<Entry>(uplinkCount)),
      _fromLeaf(leafCount, std::vector<Entry>(uplinkCount)),
      _nextFeedback(leafCount, 0), _egress(*this)
{
    if (leafId >= leafCount || uplinkCount == 0 || uplinkCount > 16 || expiry == 0) {
        throw std::invalid_argument("Invalid leaf ID, uplink count, or feedback expiry");
    }
}

bool LeafSwitch::fresh(const Entry &entry, simtime_picosec now) const
{
    return entry.valid && now >= entry.updated && now - entry.updated < _expiry;
}

LeafSwitch::Entry LeafSwitch::snapshot(const Entry &entry, simtime_picosec now) const
{
    return fresh(entry, now) ? entry : Entry{};
}

LeafSwitch::Entry LeafSwitch::toLeaf(uint32_t destination, uint32_t uplink,
                                   simtime_picosec now) const
{
    return snapshot(_toLeaf.at(destination).at(uplink), now);
}

LeafSwitch::Entry LeafSwitch::fromLeaf(uint32_t source, uint32_t uplink,
                                     simtime_picosec now) const
{
    return snapshot(_fromLeaf.at(source).at(uplink), now);
}

void LeafSwitch::receivePacket(Packet &packet)
{
    const auto now = EventList::Get().now();
    const auto &metadata = packet.congaMetadata();
    const uint32_t peer = metadata.sourceLeaf;
    if (metadata.destinationLeaf == _leafId && peer < _fromLeaf.size() && peer != _leafId) {
        if (metadata.valid && metadata.lbTag < _uplinkCount && metadata.ce < 8) {
            Entry &entry = _fromLeaf[peer][metadata.lbTag];
            // Preserve pending priority until sent, even if later observations agree.
            entry.pending = !fresh(entry, now) || entry.pending || entry.metric != metadata.ce;
            entry.valid = true;
            entry.metric = metadata.ce;
            entry.updated = now;
        }
        if (metadata.feedbackValid && metadata.feedbackLbTag < _uplinkCount &&
            metadata.feedbackMetric < 8) {
            Entry &entry = _toLeaf[peer][metadata.feedbackLbTag];
            entry.valid = true;
            entry.metric = metadata.feedbackMetric;
            entry.updated = now;
        }
    }
    packet.sendOn(); // Do not access packet afterwards: its receiver may free it.
}

bool LeafSwitch::stampFeedback(Packet &packet, uint32_t peer, simtime_picosec now)
{
    // First scan pending entries, then refresh unchanged entries. Each peer has
    // its own cursor; both scans wrap deterministically without drawing RNGs.
    for (bool pendingOnly : {true, false}) {
        for (uint32_t offset = 0; offset < _uplinkCount; ++offset) {
            const uint32_t uplink = (_nextFeedback[peer] + offset) % _uplinkCount;
            Entry &entry = _fromLeaf[peer][uplink];
            if (fresh(entry, now) && (!pendingOnly || entry.pending)) {
                packet.setCongaFeedback(uplink, entry.metric);
                entry.pending = false;
                _nextFeedback[peer] = (uplink + 1) % _uplinkCount;
                return true;
            }
        }
    }
    return false;
}

void LeafSwitch::sendToFabric(Packet &packet)
{
    // The leaf owns feedback selection, including clearing the ACK factory's
    // immediate-echo fallback when no fresh table entry exists.
    packet.clearCongaFeedback();
    const auto &metadata = packet.congaMetadata();
    if (metadata.sourceLeaf == _leafId && metadata.destinationLeaf < _fromLeaf.size() &&
        metadata.destinationLeaf != _leafId) {
        stampFeedback(packet, metadata.destinationLeaf, EventList::Get().now());
    }
    packet.sendOn();
}
