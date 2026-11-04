#include "output-options.h"

namespace {
    bool verboseEnabled = false;
}

bool output::verbose() { return verboseEnabled; }
void output::setVerbose(bool enabled) { verboseEnabled = enabled; }
