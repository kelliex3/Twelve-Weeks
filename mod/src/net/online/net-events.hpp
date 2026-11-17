#pragma once

#include "net-protocol.hpp"
#include <vector>
#include <cstring>
#include <algorithm>
#include <unordered_set>

constexpr size_t kMaxEventPayload = 200;

struct NetPendingEvent {
    uint32_t id;
    uint8_t  type;
    uint8_t  senderSlot;
    uint8_t  targetSlot;
    uint8_t  size;
    uint8_t  data[kMaxEventPayload];
    float    firstSend;
    float    lastSend;
};

class NetEventChannel {
public:
    void Reset()
    {
        m_pending.clear();
        m_acks.clear();
        memset(m_recentIds, 0, sizeof(m_recentIds));
        m_recentIdx = 0;
        m_recentSet.clear();
        m_nextId = 1;
    }

    void Queue(uint8_t type, uint8_t senderSlot, uint8_t targetSlot,
               const void* data, uint8_t size, float now)
    {
        if (size > kMaxEventPayload) size = kMaxEventPayload;
        NetPendingEvent e{};
        e.id = m_nextId++;
        e.type = type;
        e.senderSlot = senderSlot;
        e.targetSlot = targetSlot;
        e.size = size;
        if (size && data) memcpy(e.data, data, size);
        e.firstSend = now;
        e.lastSend = 0.0f;
        m_pending.push_back(e);
    }

    size_t WriteBlock(uint8_t* buf, size_t bufSize, float now)
    {
        size_t w = 0;

        // Abandon events older than kEventExpireSeconds
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [now](const NetPendingEvent& e) { return now - e.firstSend > kEventExpireSeconds; }),
            m_pending.end());

        // ACKs
        size_t ackC = (std::min)(m_acks.size(), static_cast<size_t>(255));
        if (w + 1 + ackC * 4 > bufSize) return 0;
        buf[w++] = static_cast<uint8_t>(ackC);
        for (size_t i = 0; i < ackC; i++) {
            memcpy(buf + w, &m_acks[i], 4); w += 4;
        }
        m_acks.erase(m_acks.begin(), m_acks.begin() + ackC);

        // Events (resend every kEventResendSeconds)
        if (w + 1 > bufSize) return w;
        size_t countPos = w++;
        uint8_t count = 0;

        for (auto& e : m_pending) {
            if (e.lastSend > 0 && now - e.lastSend < kEventResendSeconds) continue;
            size_t needed = 4 + 1 + 1 + 1 + 1 + e.size;
            if (w + needed > bufSize || count >= 255) break;

            memcpy(buf + w, &e.id, 4); w += 4;
            buf[w++] = e.type;
            buf[w++] = e.senderSlot;
            buf[w++] = e.targetSlot;
            buf[w++] = e.size;
            memcpy(buf + w, e.data, e.size); w += e.size;

            e.lastSend = now;
            count++;
        }
        buf[countPos] = count;
        return w;
    }

    template <typename Sink>
    void ReadBlock(const uint8_t* buf, size_t size, Sink&& sink)
    {
        size_t o = 0;
        if (o >= size) return;

        // ACKs for our outgoing events
        uint8_t ackCount = buf[o++];
        for (uint8_t i = 0; i < ackCount; i++) {
            if (o + 4 > size) return;
            uint32_t ackId;
            memcpy(&ackId, buf + o, 4); o += 4;
            m_pending.erase(
                std::remove_if(m_pending.begin(), m_pending.end(),
                    [ackId](const NetPendingEvent& e) { return e.id == ackId; }),
                m_pending.end());
        }

        // Incoming events
        if (o >= size) return;
        uint8_t count = buf[o++];

        for (uint8_t i = 0; i < count; i++) {
            if (o + 8 > size) return;
            uint32_t id;
            memcpy(&id, buf + o, 4); o += 4;
            uint8_t type   = buf[o++];
            uint8_t sender = buf[o++];
            uint8_t target = buf[o++];
            uint8_t sz     = buf[o++];
            if (o + sz > size) return;

            m_acks.push_back(id);

            if (!Seen(id))
                sink(type, sender, target, buf + o, sz);

            o += sz;
        }
    }

    size_t PendingCount() const { return m_pending.size(); }

private:
    static constexpr int kRecentIdCap = 4096;

    bool Seen(uint32_t id)
    {
        if (m_recentSet.count(id)) return true;
        m_recentSet.erase(m_recentIds[m_recentIdx]);
        m_recentIds[m_recentIdx] = id;
        m_recentIdx = (m_recentIdx + 1) % kRecentIdCap;
        m_recentSet.insert(id);
        return false;
    }

    std::vector<NetPendingEvent> m_pending;
    std::vector<uint32_t>        m_acks;
    uint32_t m_recentIds[kRecentIdCap] = {};
    int      m_recentIdx = 0;
    std::unordered_set<uint32_t> m_recentSet;
    uint32_t m_nextId = 1;
};
