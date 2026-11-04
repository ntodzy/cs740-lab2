/*
 * Simulator eventlist header
 */
#ifndef EVENTLIST_H
#define EVENTLIST_H

#include "htsim.h"
#include "loggertypes.h"

#include <queue>
#include <map>
#include <string>
#include <unordered_map>

class EventSource : public Logged
{
    public:
        EventSource(const std::string &name) : Logged(name) {};
        virtual ~EventSource() {};
        virtual void doNextEvent() = 0;
};

class EventList
{
    public:
        // Returns the eventlist instance.
        static EventList& Get();

        // End simulation at endtime (rather than forever)
        void setEndtime(simtime_picosec endtime);

        // Returns true if it did anything, false if there's nothing to do.
        bool doNextEvent();

        // Enqueue future events into the simulator.
        void sourceIsPending(EventSource &src, simtime_picosec when);
        void sourceIsPendingRel(EventSource &src, simtime_picosec timefromnow)
        {
            sourceIsPending(src, now() + timefromnow);
        }

        // Returns current simulation time.
        inline simtime_picosec now() {return _lasteventtime;}

        uint64_t _nEventsProcessed;
        std::unordered_map<std::string,std::pair<uint32_t,double> > _stats;

    private:
        EventList(){}; // Cannot be called, singleton instance.
        ~EventList(){};
        EventList(const EventList&); // Copy constructor too.
        EventList& operator=(const EventList&); // Assignment operator too.

        static EventList *instance;

        struct PendingEvent {
            simtime_picosec time;
            uint64_t sequence;
            EventSource *source;
        };
        struct LaterEvent {
            bool operator()(const PendingEvent &a, const PendingEvent &b) const {
                return a.time > b.time || (a.time == b.time && a.sequence > b.sequence);
            }
        };
        // Sequence preserves the multimap's insertion order at equal timestamps,
        // including events scheduled by a callback for the current time.
        std::priority_queue<PendingEvent, std::vector<PendingEvent>, LaterEvent> _pendingsources;
        uint64_t _nextSequence = 0;
        simtime_picosec _endtime;
        simtime_picosec _lasteventtime;
};

#endif /* EVENTLIST_H */
