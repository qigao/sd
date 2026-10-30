#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "stable-diffusion.h"

// sd.cpp provides preview frames via a native callback. The preview is
// best-effort, unlike the inference result: never allocate unbounded PNG/JPEG/base64
// media or admit malformed dimensions into the callback encoder.
constexpr std::uint64_t k_sd_app_preview_max_pixels = 4ULL * 1024 * 1024;
constexpr std::uint64_t k_sd_app_preview_max_encoded_bytes = 8ULL * 1024 * 1024;

inline bool sd_app_preview_frame_in_budget(std::int64_t width,
                                            std::int64_t height,
                                            std::int64_t channels) noexcept {
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4)
        return false;
    return static_cast<std::uint64_t>(width) <=
           k_sd_app_preview_max_pixels / static_cast<std::uint64_t>(height);
}

// The same bounded encoder/exception boundary is used by native async jobs
// and the inherited synchronous SDAPI preview. Only the encoded preview is
// optional: rejecting it must never fail the underlying sd.cpp generation.
constexpr std::uint64_t k_sd_app_preview_max_base64_bytes =
    ((k_sd_app_preview_max_encoded_bytes + 2ULL) / 3ULL) * 4ULL;

template <typename Encode, typename Base64>
bool sd_app_try_encode_preview(const sd_image_t& frame, Encode&& encode,
                               Base64&& to_base64, std::string& published) noexcept {
    if (!frame.data ||
        !sd_app_preview_frame_in_budget(frame.width, frame.height, frame.channel))
        return false;
    try {
        auto bytes = std::forward<Encode>(encode)(frame);
        if (bytes.empty() || bytes.size() > k_sd_app_preview_max_encoded_bytes)
            return false;
        auto encoded = std::forward<Base64>(to_base64)(bytes);
        if (encoded.empty() || encoded.size() > k_sd_app_preview_max_base64_bytes)
            return false;
        published.swap(encoded); // Publication only after all work succeeds.
        return true;
    } catch (...) {
        // Never let an allocation/codec exception unwind across the C ABI.
        return false;
    }
}
