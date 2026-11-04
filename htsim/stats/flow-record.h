#ifndef FLOW_RECORD_H
#define FLOW_RECORD_H

#include "../htsim.h"

// Transport-independent completion event, using the generator's routing ID.
// Times are picoseconds internally; writers choose explicit output units.
struct FlowCompletion {
    uint64_t flowId;
    uint32_t source;
    uint32_t destination;
    uint64_t sizeBytes;
    simtime_picosec start;
    simtime_picosec finish;
};

#endif // FLOW_RECORD_H
