#pragma once
// -----------------------------------------------------------------------------
//  The droplight beam's color, inside the game's own effect file
// -----------------------------------------------------------------------------
//  fx_horadric_light.particles (PopcornFX, compiled) has no color input the
//  game can set. Its beam color is the constant RGBA (0.9296, 0.9021, 1.0,
//  0.8275), stored as plain 32-bit floats in every compiled form of the
//  effect:
//    - 4-float vectors, R G B A in order (two of them side by side), some with
//      alpha 0 (the faded end);
//    - the 8-wide constant pools, one slot per value: A, R, G in slots 9 to
//      11. Blue there is the pool's shared 1.0 (slot 1), which other math in
//      the effect uses too, so it is left alone.
//  A recolor writes the new red and green over every stored R and G, the new
//  blue over the blue of every R G B vector, and scales every stored alpha by
//  the brightness asked for. Nothing else in the file changes, so its size and
//  layout stay the same.
//  Brightness matters for the hue: the game's beam is bright enough that its
//  core clips to white, and a saturated color at that brightness clips too
//  (purple's red and blue both hit full and read as pink), so colored beams
//  are drawn dimmer than the white one.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace beam {

inline constexpr std::uint32_t RedBits   = 0x3F6DFCE9u;  // 0.9296
inline constexpr std::uint32_t GreenBits = 0x3F66ECCDu;  // 0.9021
inline constexpr std::uint32_t OneBits   = 0x3F800000u;  // 1.0, the blue of each R G B vector
inline constexpr std::uint32_t AlphaBits = 0x3F53D3D5u;  // 0.8275
inline constexpr float         SourceAlpha = 0.8275f;

struct ColorSites {
    std::vector<std::size_t> red;
    std::vector<std::size_t> green;
    std::vector<std::size_t> blue;  // only where the vector form spells it out
    std::vector<std::size_t> alpha;
    [[nodiscard]] auto Found() const -> bool { return !red.empty() && red.size() == green.size() && !blue.empty(); }
};

inline auto ReadBits(const std::vector<std::uint8_t>& bytes, std::size_t offset) -> std::uint32_t {
    std::uint32_t value = 0;
    if (offset + 4 <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Byte aligned: the pools sit at odd offsets.
inline auto FindColor(const std::vector<std::uint8_t>& bytes) -> ColorSites {
    ColorSites sites;
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        const std::uint32_t value = ReadBits(bytes, offset);
        if (value == RedBits) {
            sites.red.push_back(offset);
            if (ReadBits(bytes, offset + 4) == GreenBits && ReadBits(bytes, offset + 8) == OneBits) sites.blue.push_back(offset + 8);
        } else if (value == GreenBits) {
            sites.green.push_back(offset);
        } else if (value == AlphaBits) {
            sites.alpha.push_back(offset);
        }
    }
    return sites;
}

// The beam in the given color (0 to 1 per channel), scaled so its strongest
// channel is 1 like the game's own beam (its blue is 1), at the given share of
// the game's beam brightness (0 to 1, through alpha).
inline auto Recolor(std::vector<std::uint8_t> bytes, const ColorSites& sites, double red, double green, double blue, double brightness)
    -> std::vector<std::uint8_t> {
    const double strongest = std::max({ red, green, blue });
    const double scale = strongest > 0.0 ? 1.0 / strongest : 1.0;
    const auto write = [&](const std::vector<std::size_t>& offsets, double channel) {
        const float value = static_cast<float>(channel * scale);
        for (std::size_t offset : offsets) {
            if (offset + 4 <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value));
        }
    };
    write(sites.red, red);
    write(sites.green, green);
    write(sites.blue, blue);
    const float alpha = SourceAlpha * static_cast<float>(std::clamp(brightness, 0.0, 1.0));
    for (std::size_t offset : sites.alpha) {
        if (offset + 4 <= bytes.size()) std::memcpy(bytes.data() + offset, &alpha, sizeof(alpha));
    }
    return bytes;
}

}  // namespace beam
