#pragma once
#include "runtime/progress_snapshot.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nwii::runtime::discord {
inline constexpr const char* application_id = "1558931378215723168";
struct Activity { std::string details, state; int64_t start = 0; };
// Only these public strings leave the game; never paths, save names or guest memory.
std::string AreaName(const char (&stage)[9]);
Activity MakeActivity(bool launcher, bool port_menu, bool show_area,
                      const progress::Snapshot& scene, int64_t start);
std::string ActivityJson(const Activity& activity, uint32_t pid, uint64_t nonce,
                         const std::string& image = {});
std::vector<uint8_t> EncodeFrame(uint32_t opcode, const std::string& json);
class Decoder {
    std::vector<uint8_t> bytes_;
public:
    bool Append(const uint8_t* data, size_t size);
    bool Pop(uint32_t& opcode, std::string& payload);
};
// UI/main thread publishes once a second; all IPC runs on a separate worker.
void Update(bool enabled, bool launcher, bool port_menu, bool show_area,
            const progress::Snapshot& scene, const std::string& image = {});
void Shutdown();
}
