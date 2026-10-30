#pragma once

#include <cstddef>

// Native generation stores base64 media. Do not allocate base64 when the
// encoded byte count cannot fit in the remaining per-task budget.
// The multiplication order avoids overflow even for SIZE_MAX.
inline size_t sd_app_max_encoded_bytes_for_base64(size_t already_encoded_b64,
                                                  size_t max_encoded_b64) noexcept {
    if (already_encoded_b64 >= max_encoded_b64)
        return 0;
    return ((max_encoded_b64 - already_encoded_b64) / 4) * 3;
}

inline bool sd_app_encoded_bytes_fit_base64(size_t encoded_bytes,
                                             size_t already_encoded_b64,
                                             size_t max_encoded_b64) noexcept {
    return encoded_bytes > 0 &&
           encoded_bytes <= sd_app_max_encoded_bytes_for_base64(
                                already_encoded_b64, max_encoded_b64);
}
