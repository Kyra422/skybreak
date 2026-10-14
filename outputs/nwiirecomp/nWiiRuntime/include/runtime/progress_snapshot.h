#pragma once
// SOUE01 only. Published by the guest thread; host UI never reads live guest RAM.
#include <array>
#include <cstdint>
#include <mutex>
namespace nwii::runtime::progress {
struct Milestone { const char *id, *nl, *en, *desc_nl, *desc_en; uint16_t flag; uint16_t prerequisite = 0; };
inline constexpr std::array<Milestone, 12> milestones = {{
    {"loftwing", "Vrije vleugels", "Free wings", "Red je Loftwing.", "Rescue your Loftwing.", 27},
    {"goddess_sword", "De roep van de godin", "The goddess calls", "Verkrijg het Godinnenzwaard.", "Obtain the Goddess Sword.", 3},
    {"skyview", "Meester van Skyview", "Skyview champion", "Voltooi de Skyview Temple.", "Complete Skyview Temple.", 5},
    {"earth_temple", "Onder de vulkaan", "Beneath the volcano", "Voltooi de Earth Temple.", "Complete the Earth Temple.", 7},
    {"harp", "Een nieuw lied", "A new song", "Ontvang de harp van Zelda.", "Receive Zelda's harp.", 9},
    {"ancient_cistern", "Gouden diepten", "Golden depths", "Voltooi de Ancient Cistern.", "Complete the Ancient Cistern.", 11},
    {"sandship", "Kapitein van het zand", "Captain of the sands", "Voltooi de Sandship.", "Complete the Sandship.", 15},
    {"fire_sanctuary", "Door het vuur", "Through the fire", "Voltooi de Fire Sanctuary.", "Complete the Fire Sanctuary.", 13},
    {"tadtones", "Het koor compleet", "The complete choir", "Verzamel alle Tadtones.", "Collect all Tadtones.", 18},
    {"sky_keep_open", "De laatste proef", "The final trial", "Open de Sky Keep met de Stone of Trials.", "Open Sky Keep with the Stone of Trials.", 22},
    {"triforce", "De drie krachten", "The three powers", "Voltooi de Triforce.", "Complete the Triforce.", 17},
    {"sky_keep", "De godin keert terug", "The goddess returns", "Voltooi de Sky Keep.", "Complete Sky Keep.", 26, 17}
}};
// Completion requires its cause: flag 26 can be set alone in edited/old test saves.
template<class ReadFlag> bool attained(const Milestone& m, ReadFlag read) {
    return read(m.flag) && (!m.prerequisite || read(m.prerequisite));
}
struct Snapshot {
    bool playing = false;
    uint32_t mask = 0;
    char stage[9] = {};
    int room = 0, layer = 0, entrance = 0;
    float pos[3] = {};
};
class Channel {
    std::mutex mutex_; Snapshot snapshot_;
public:
    void publish(const Snapshot& s) { std::lock_guard lock(mutex_); snapshot_ = s; }
    Snapshot get() { std::lock_guard lock(mutex_); return snapshot_; }
};
inline Channel channel;
}
