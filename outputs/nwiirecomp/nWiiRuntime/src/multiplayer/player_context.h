#pragma once
// Experimental SOUE01 co-op foundation. Not wired into gameplay: a second
// independently initialized actor and actor/item ownership hooks are required.
#include <array>
#include <cstdint>
#include <exception>

namespace nwii::runtime::multiplayer {
struct GuestGlobalsIO {
    void* user=nullptr;
    bool (*read)(void*,uint32_t,uint32_t&) noexcept=nullptr;
    bool (*write)(void*,uint32_t,uint32_t) noexcept=nullptr;
};
// Serial guest-thread scope only. This does not make the guest engine thread-safe.
// GetLink and GetLink2 are aliases of the current actor in normal gameplay.
// The controller's corresponding Core and dPad::ex_c must switch together.
class PlayerContextScope {
public:
    PlayerContextScope()=default;
    PlayerContextScope(const PlayerContextScope&)=delete;
    PlayerContextScope& operator=(const PlayerContextScope&)=delete;
    ~PlayerContextScope(){if(!leave())std::terminate();}
    bool enter(GuestGlobalsIO io,uint32_t actor,unsigned channel) noexcept {
        if(active_ || !io.read || !io.write || channel>=4 || !ram_pointer(actor))return false;
        uint32_t core=0;
        if(!io.read(io.user,0x805CB0B8+4*channel,core) || !ram_pointer(core))return false;
        for(unsigned i=0;i<locations.size();++i)
            if(!io.read(io.user,locations[i],saved_[i]))return false;
        io_=io;active_=true;
        const std::array<uint32_t,5> values={actor,actor,0x80597A80+0x22DC*channel,channel,core};
        for(unsigned i=0;i<locations.size();++i)
            if(!io_.write(io_.user,locations[i],values[i])) {leave();return false;}
        return true;
    }
    bool leave() noexcept {
        if(!active_)return true;
        bool ok=true;
        for(unsigned i=locations.size();i>0;--i)
            ok=io_.write(io_.user,locations[i-1],saved_[i-1]) && ok;
        // A failed restore leaves the scope active so the caller can retry.
        // Never resume guest execution after a failed restore.
        if(ok)active_=false;
        return ok;
    }
    bool active()const noexcept{return active_;}
    static bool ram_pointer(uint32_t p)noexcept {
        return !(p&3) && ((p>=0x80000000 && p<0x81800000) || (p>=0x90000000 && p<0x94000000));
    }
    // SOUE01 symbol map: LINK, LINK2, dPad::m_current_ex,
    // mPad::g_currentCoreId, mPad::g_currentCore.
    static constexpr std::array<uint32_t,5> locations={0x8057578C,0x805757B8,0x80571C60,0x80575C54,0x80575C58};
private:
    GuestGlobalsIO io_{};
    std::array<uint32_t,5> saved_{};
    bool active_=false;
};
} // namespace nwii::runtime::multiplayer
