#pragma once
#include <perfetto.h>
#include <string>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("skeleton")
        .SetDescription("category relevant about the tracking of the full "
                        "function to thin the image"),
    perfetto::Category("simple_point")
        .SetDescription("Simple point tracking part"),
    perfetto::Category("sequential_recheck")
        .SetDescription("Sequential recheck tracking part"));

namespace skel {

// Call this ONCE at the very start of a Python script
void init_tracing();

// Call this to start recording
void start_tracing();

// Call this to stop recording and flush it to a file to disk
void stop_tracing(const std::string &filename);

} // namespace skel
