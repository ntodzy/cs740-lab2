#include "../eventlist.h"

#include <stdexcept>
#include <vector>
#include <iostream>

class RecordedEvent : public EventSource {
public:
    RecordedEvent(int id, std::vector<int> &seen)
        : EventSource("recorded-event"), _id(id), _seen(seen) {}
    void doNextEvent() override {
        _seen.push_back(_id);
        if (schedule) EventList::Get().sourceIsPending(*schedule, EventList::Get().now());
    }
    EventSource *schedule = nullptr;
private:
    int _id;
    std::vector<int> &_seen;
};

int main()
{
    EventList &events = EventList::Get();
    events.setEndtime(20);
    std::vector<int> seen;
    RecordedEvent first(1, seen), second(2, seen), third(3, seen);
    RecordedEvent nested(4, seen), cutoff(5, seen), excluded(6, seen);
    first.schedule = &nested;
    // Insert out of timestamp order; repeat one source at the same timestamp.
    events.sourceIsPending(cutoff, 20);
    events.sourceIsPending(first, 10);
    events.sourceIsPending(second, 10);
    events.sourceIsPending(third, 5);
    events.sourceIsPending(second, 10);
    events.sourceIsPending(excluded, 21);
    while (events.doNextEvent()) {}
    if (seen != std::vector<int>({3, 1, 2, 2, 4, 5}) ||
        events.now() != 20 || events._nEventsProcessed != 6 || events.doNextEvent()) {
        throw std::runtime_error("Event order, reentrant scheduling, or cutoff changed");
    }
    std::cout << "Event ordering and cutoff checks passed\n";
}
