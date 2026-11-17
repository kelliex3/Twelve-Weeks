#include "net-timer.hpp"

#include <cmath>

void NetTimer::Start(float seconds)
{
    m_duration = seconds > 0.0f ? seconds : 0.0f;
    m_remaining = m_duration;
    m_running = m_duration > 0.0f;
    m_fired = false;
}

void NetTimer::Stop()
{
    m_running = false;
    m_remaining = 0.0f;
}

void NetTimer::Clear()
{
    m_running = false;
    m_remaining = 0.0f;
    m_duration = 0.0f;
    m_fired = false;
}

bool NetTimer::Tick(float dt)
{
    if (!m_running) return false;
    m_remaining -= dt;
    if (m_remaining > 0.0f) return false;
    m_remaining = 0.0f;
    m_running = false;
    if (m_fired) return false;
    m_fired = true;
    return true;
}

bool  NetTimer::Running()   const { return m_running; }
bool  NetTimer::Expired()   const { return m_fired; }
float NetTimer::Remaining() const { return m_remaining; }
float NetTimer::Duration()  const { return m_duration; }
int   NetTimer::Ceil()      const { return static_cast<int>(ceilf(m_remaining)); }

NetTimerSync NetTimer::Snapshot() const
{
    return NetTimerSync{ static_cast<uint8_t>(m_running ? 1 : 0), m_remaining, m_duration };
}

void NetTimer::ApplySync(const NetTimerSync& s)
{
    m_running = (s.running != 0);
    m_remaining = s.remaining;
    m_duration = s.duration;
    m_fired = (!m_running && m_remaining <= 0.0f);
}