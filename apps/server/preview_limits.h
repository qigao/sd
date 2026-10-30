#pragma once

#include <cstdint>

// sd.cpp provides preview frames via a native callback. The preview is
// best-effort, unlike the inference result: never allocate unbounded PNG/base64
// media or admit malformed dimensions into the callback encoder.
constexpr std::uint64_t k_sd_app_preview_max_pixels = 4ULL * 1024 * 1024;
constexpr std::uint64_t k_sd_app_preview_max_png_bytes = 8ULL * 1024 * 1024;

inline bool sd_app_preview_frame_in_budget(std::int64_t width,
                                            std::int64_t height,
                                            std::int64_t channels) noexcept {
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4)
        return false;
    return static_cast<std::uint64_t>(width) <=
           k_sd_app_preview_max_pixels / static_cast<std::uint64_t>(height);
}
