#pragma once
// Ported from the D4 browser. Optional live-reporting hooks for batch export
// pipelines. When a sink is supplied the pipeline reports per-item progress and
// human-readable log lines here INSTEAD of popping its own modal progress dialog,
// and polls `canceled` between items — this is what feeds the Bulk Extract tab's
// live console. In that tab `canceled` also doubles as the pause gate: workers
// sleep inside it while the run is paused, so pausing is instant and free.
// A null sink keeps in-tab dialog behaviour.

#include <QString>

#include <functional>

struct BatchSink {
    std::function<void(int done, int total)> progress;   // called per item
    std::function<void(const QString& line)> log;        // one line per notable event
    std::function<bool()> canceled;                      // polled between items
};
