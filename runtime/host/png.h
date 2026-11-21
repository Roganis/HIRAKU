// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// PNG pictures read for mods (docs/MODS.md: a picture of the game's given as a PNG): 8- and 16-bit
// greyscale, RGB, palette (with tRNS) and alpha, not interlaced; through zlib. And written, for the
// screenshots (host/screenshot.h): 8-bit RGB, not interlaced.
#include <cstdint>
#include <string>
#include <vector>

namespace hk::png {

// The picture in `file` as RGBA (width x height x 4); false with a message in err
bool decode(const std::vector<uint8_t> &file, int &width, int &height, std::vector<uint8_t> &rgba, std::string &err);
// A PNG file of the picture rgb (0x00RRGGBB a pixel, as the cores give it), width x height: 8-bit RGB,
// no alpha; empty if the size is none or over decode's 8192 pixels a side
std::vector<uint8_t> encode(const uint32_t *rgb, int width, int height);

}  // namespace hk::png
