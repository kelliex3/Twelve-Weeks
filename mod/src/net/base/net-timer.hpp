#pragma once

#include <cstdint>

struct NetTimerSync {
    uint8_t running;     // 1 while counting down
    float   remaining;   // seconds left
    float   duration;    // seconds the countdown started from
};

class NetTimer {
public:
    void Start(float seconds);
    void Stop();                 // cancel, keep duration
    void Clear();                // full reset

    // Advance the countdown. Returns true on the single frame it reaches zero,
    // so callers fire the "done" action without tracking the edge themselves.
    bool Tick(float dt);

    bool  Running()   const;
    bool  Expired()   const;
    float Remaining() const;
    float Duration()  const;
    int   Ceil()      const;

    NetTimerSync Snapshot() const;
    void         ApplySync(const NetTimerSync& s);

private:
    float m_duration = 0.0f;
    float m_remaining = 0.0f;
    bool  m_running = false;
    bool  m_fired = false;
};