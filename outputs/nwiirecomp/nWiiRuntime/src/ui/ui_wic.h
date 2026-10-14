#pragma once
// UI13: Windows Imaging Component helpers (part of Windows, nothing downloaded): JPEG decode for
// the THP background frame and PNG encode for the UI self-test screenshots.
#include <cstdint>
#include <string>
#include <vector>

#include "ui_assets.h"

namespace nwii::ui {

bool DecodeImageWic(const std::vector<uint8_t>& encoded, Image& out, std::string* err = nullptr);
bool SavePngWic(const std::string& path_utf8, const Image& img, std::string* err = nullptr);

} // namespace nwii::ui
