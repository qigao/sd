#include "stable-diffusion.h"
#include "tinytest.hpp"

#include <cstring>
#include <string_view>
#include <vector>

spec("sd.cpp inference library integration")
{
    it("links the pinned public C ABI without loading weights")
    {
        const char* commit = sd_commit();
        check_true(commit != nullptr);
        if (commit != nullptr)
        {
            constexpr char expected[] = SD_APP_SDCPP_EXPECTED_COMMIT;
            const auto length = std::strlen(commit);
            check_true(length >= 7 && length <= sizeof(expected) - 1);
            if (length >= 7 && length <= sizeof(expected) - 1)
                check_equal(std::strncmp(commit, expected, length), 0);
        }

        const char* version = sd_version();
        check_true(version != nullptr);
        if (version != nullptr)
            check_true(version[0] != '\0');

        // These functions are in the inference API object, not only version.cpp.
        // Referencing them makes the smoke executable resolve the inference
        // library's real link dependencies while remaining independent of GPUs
        // and external model assets at runtime.
        sd_ctx_params_t context_params;
        sd_ctx_params_init(&context_params);
        check_true(context_params.model_path == nullptr);
        check_true(context_params.diffusion_model_path == nullptr);
        check_true(context_params.vae_path == nullptr);

        sd_img_gen_params_t image_params;
        sd_img_gen_params_init(&image_params);
        check_true(image_params.width > 0);
        check_true(image_params.height > 0);
        check_true(image_params.batch_count > 0);

        sd_vid_gen_params_t video_params;
        sd_vid_gen_params_init(&video_params);
        check_true(video_params.width > 0);
        check_true(video_params.height > 0);
        check_true(video_params.video_frames > 0);
    }

    it("discovers configured devices through the public buffer API")
    {
        const auto required = sd_list_devices(nullptr, 0);
        std::vector<char> devices(required + 1, '?');
        check_equal(sd_list_devices(devices.data(), devices.size()), required);
        check_equal(devices[required], '\0');
        check_equal(std::strlen(devices.data()), required);

#if SD_APP_EXPECT_CPU
        // A dynamic GGML build must load its CPU module at runtime. Merely
        // linking the core shared library cannot satisfy this assertion.
        const std::string_view listing(devices.data(), required);
        check_true(listing.find("CPU\t") == 0 ||
                   listing.find("\nCPU\t") != std::string_view::npos);
#endif

        char bounded[] = {'?', '!'};
        check_equal(sd_list_devices(bounded, 1), required);
        check_equal(bounded[0], '\0');
        check_equal(bounded[1], '!');
        bounded[0] = '?';
        check_equal(sd_list_devices(bounded, 0), required);
        check_equal(bounded[0], '?');
    }

    it("rejects generation without a context and clears the result")
    {
        sd_img_gen_params_t image_params;
        sd_img_gen_params_init(&image_params);
        sd_image_generation_result_t image_sentinel{};
        auto* image_result = &image_sentinel;
        check_false(generate_image(nullptr, &image_params, &image_result));
        check_true(image_result == nullptr);

        sd_vid_gen_params_t video_params;
        sd_vid_gen_params_init(&video_params);
        sd_video_generation_result_t video_sentinel{};
        auto* video_result = &video_sentinel;
        check_false(generate_video(nullptr, &video_params, &video_result));
        check_true(video_result == nullptr);
    }
}
