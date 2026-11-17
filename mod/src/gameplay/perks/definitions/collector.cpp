#include "collector.hpp"

#include "net-debug.hpp"
#include "net-session.hpp"

#include <match.hpp>

namespace gameplay {

    float kCollectorPartsBonus = 0.1f;

    void CollectorOnPartsAwarded(int parts)
    {
        TO1_LOG("[perk] entered CollecterOnpartsAwarded\n");
        int bonus = static_cast<int>(std::ceil(parts * kCollectorPartsBonus / 5.0)) * 5; // round up to the nearest 5 or 10 int
        net::match::Get().stats[net::session::MySlot()].parts += bonus;
        TO1_LOG("[perk] Collector earned %f percent of %d in bonus -> %d parts awarded\n", kCollectorPartsBonus*100.0f, parts, bonus);
    }
}