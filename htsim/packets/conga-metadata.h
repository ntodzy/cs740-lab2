#ifndef CONGA_METADATA_H
#define CONGA_METADATA_H

#include <cstdint>

// Forward-path measurement only; leaf addressing and feedback come later.
struct CongaMetadata {
    bool valid = false;
    uint32_t lbTag = 0;
    uint32_t ce = 0;
};

#endif // CONGA_METADATA_H
