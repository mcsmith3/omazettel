#pragma once

#include <QElapsedTimer>
#include <QtGlobal>

#include <cstdio>

// OMAZETTEL_TIMING=1 prints, on stderr, when each part of starting up is
// done and how long the slow steps take, to see where a slow start goes.
namespace StartupTiming {

inline bool enabled() {
    static const bool on = qEnvironmentVariableIntValue("OMAZETTEL_TIMING") == 1;
    return on;
}

// Time since the app started (the first call).
inline qint64 sinceStart() {
    static QElapsedTimer clock;
    static const bool started = (clock.start(), true);
    Q_UNUSED(started);
    return clock.elapsed();
}

inline void mark(const char *what) {
    if (!enabled())
        return;
    std::fprintf(stderr, "omazettel timing: %7lld ms  %s\n", static_cast<long long>(sinceStart()), what);
    std::fflush(stderr);
}

// Reports how long the scope it lives in took.
class Step {
public:
    explicit Step(const char *what) : m_what(what) {
        if (enabled())
            m_timer.start();
    }
    ~Step() {
        if (!enabled())
            return;
        std::fprintf(stderr, "omazettel timing: %7lld ms  %s took %lld ms\n",
                     static_cast<long long>(sinceStart()), m_what, static_cast<long long>(m_timer.elapsed()));
        std::fflush(stderr);
    }

private:
    const char *m_what;
    QElapsedTimer m_timer;
};

}
