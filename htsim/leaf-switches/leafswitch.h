#ifndef LEAF_SWITCH_H
#define LEAF_SWITCH_H

#include "../packets/network.h"

// State shared by every flow through one leaf. Routes own neither hook.
class LeafSwitch : public PacketSink
{
public:
    struct Entry {
        bool valid = false;
        uint32_t metric = 0;
        simtime_picosec updated = 0;
        bool pending = false; // New/changed observation awaiting feedback.
    };

    LeafSwitch(uint32_t leafId, uint32_t leafCount, uint32_t uplinkCount,
               simtime_picosec expiry = timeFromUs(10000));
    LeafSwitch(const LeafSwitch &) = delete;
    LeafSwitch &operator=(const LeafSwitch &) = delete;
    uint32_t leafId() const { return _leafId; }

    // Incoming fabric hook: process observation and feedback, then forward.
    void receivePacket(Packet &packet) override;
    // Outgoing fabric hook: replace endpoint echo with table-selected feedback.
    void sendToFabric(Packet &packet);
    PacketSink &egress() { return _egress; }

    // Return snapshots; expired entries are invalid, distinct from metric zero.
    Entry toLeaf(uint32_t destination, uint32_t uplink, simtime_picosec now) const;
    Entry fromLeaf(uint32_t source, uint32_t uplink, simtime_picosec now) const;

private:
    class Egress : public PacketSink {
    public:
        explicit Egress(LeafSwitch &leaf) : _leaf(leaf) {}
        void receivePacket(Packet &packet) override { _leaf.sendToFabric(packet); }
    private:
        LeafSwitch &_leaf;
    };

    bool fresh(const Entry &entry, simtime_picosec now) const;
    Entry snapshot(const Entry &entry, simtime_picosec now) const;
    bool stampFeedback(Packet &packet, uint32_t peer, simtime_picosec now);

    uint32_t _leafId;
    uint32_t _uplinkCount;
    simtime_picosec _expiry;
    std::vector<std::vector<Entry>> _toLeaf;
    std::vector<std::vector<Entry>> _fromLeaf;
    std::vector<uint32_t> _nextFeedback;
    Egress _egress;
};

#endif // LEAF_SWITCH_H
