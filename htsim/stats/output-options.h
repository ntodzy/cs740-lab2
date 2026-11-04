#ifndef OUTPUT_OPTIONS_H
#define OUTPUT_OPTIONS_H

// Human-readable streaming diagnostics are opt-in. Structured experiment
// records are independent of this setting and remain enabled in either mode.
namespace output {
    bool verbose();
    void setVerbose(bool enabled);
}

#endif // OUTPUT_OPTIONS_H
