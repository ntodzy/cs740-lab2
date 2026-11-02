#ifndef CONGA_METADATA_H
#define CONGA_METADATA_H

#include <cstdint>

// Each direction measures its own path and can carry feedback about the other.
struct CongaMetadata {
    enum : uint32_t { UNKNOWN_LEAF = UINT32_MAX };
    bool valid = false;
    uint32_t sourceLeaf = UNKNOWN_LEAF;
    uint32_t destinationLeaf = UNKNOWN_LEAF;
    uint32_t lbTag = 0;
    uint32_t ce = 0;
    bool feedbackValid = false;
    uint32_t feedbackLbTag = 0;
    uint32_t feedbackMetric = 0;

    bool hasLeafIds() const {
        return sourceLeaf != UNKNOWN_LEAF && destinationLeaf != UNKNOWN_LEAF;
    }
};

#endif // CONGA_METADATA_H
