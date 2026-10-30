// Extracted from main.cpp during server refactor.

#include "async_jobs.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include "common/log.h"
#include "common/media_io.h"
#include "common/resource_owners.hpp"
#include "workflow_execution.h"
#include "preview_limits.h"

class JobCancellationMonitor {
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;

public:
    // The caller holds sd_ctx_mutex until this monitor has joined, keeping the
    // context alive and preventing cancellation from reaching another request.
    JobCancellationMonitor(sd_ctx_t* ctx, const AsyncGenerationJob& job)
        : thread_([this, ctx, &job]() {
              std::unique_lock<std::mutex> lock(mutex_);
              while (!done_) {
                  if (job.cancel_requested.load()) {
                      // Each generation pass resets the library flag, including ADetailer.
                      sd_cancel_generation(ctx, SD_CANCEL_ALL);
                  }
                  cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() { return done_; });
              }
          }) {}

    ~JobCancellationMonitor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
};

static void preview_callback(int step, int frame_count, sd_image_t* frames, bool is_noisy, void* data) {
    (void)is_noisy;
    if (frame_count <= 0 || frames == nullptr || data == nullptr ||
        frames[0].data == nullptr) {
        return;
    }
    const sd_image_t& frame = frames[0];
    if (!sd_app_preview_frame_in_budget(frame.width, frame.height, frame.channel))
        return;

    // This callback is invoked from the sd.cpp C ABI. Encoding, base64 and
    // metadata/mutex operations can throw (for example bad_alloc). Never let
    // an exception unwind across the native callback boundary or tear down
    // an otherwise valid generation. Preview is best-effort, not an output.
    try {
        const sd_preview_info_t info = sd_get_preview_info();
        std::string encoded;
        if (!sd_app_try_encode_preview(
                frame,
                [](const sd_image_t& image) {
                    return encode_image_to_vector(EncodedImageFormat::PNG,
                                                  image.data, image.width, image.height,
                                                  image.channel, "", 80);
                },
                [](const auto& bytes) { return base64_encode(bytes); }, encoded))
            return;
        auto* job = static_cast<AsyncGenerationJob*>(data);
        std::lock_guard<std::mutex> lock(job->preview_mutex);
        job->preview_b64 = std::move(encoded);
        job->preview_step = step < 0
            ? (step == std::numeric_limits<int>::min() ? std::numeric_limits<int>::max() : -step)
            : step;
        job->preview_pass = info.sample_pass;
        job->preview_total_steps = info.total_steps;
    } catch (...) {
        // Best-effort preview failure must not escape the native callback.
    }
}

static void clear_preview_callback() {
    sd_set_preview_callback(nullptr, PREVIEW_NONE, 1, false, false, nullptr);
}

static void set_preview_callback(AsyncGenerationJob& job) {
    preview_t mode = str_to_preview(job.kind == AsyncJobKind::VidGen
                                        ? job.vid_gen.preview_mode.c_str()
                                        : job.img_gen.preview_mode.c_str());
    if (mode == PREVIEW_COUNT || mode == PREVIEW_NONE) {
        return;
    }
    int interval = job.kind == AsyncJobKind::VidGen
                       ? job.vid_gen.preview_interval
                       : job.img_gen.preview_interval;
    if (interval <= 0) {
        interval = 1;
    }
    sd_set_preview_callback(preview_callback, mode, interval, true, false, &job);
}

class PreviewCallbackGuard {
public:
    explicit PreviewCallbackGuard(AsyncGenerationJob& job) { set_preview_callback(job); }
    ~PreviewCallbackGuard() { clear_preview_callback(); }
    PreviewCallbackGuard(const PreviewCallbackGuard&) = delete;
    PreviewCallbackGuard& operator=(const PreviewCallbackGuard&) = delete;
};

bool execute_img_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::vector<std::string>& output_images,
                         json& output_info,
                         std::string& error_message) {
    output_info                = json::array();
    sd_img_gen_params_t params = job.img_gen.to_sd_img_gen_params_t();

    SDImageVec results;

    {
        std::lock_guard<std::mutex> lock(*runtime.sd_ctx_mutex);
        if (job.cancel_requested.load()) {
            return false;
        }
        JobCancellationMonitor cancellation(runtime.sd_ctx, job);
        PreviewCallbackGuard preview(job);
        sd_image_generation_result_t* generated = nullptr;
        const bool generated_ok = generate_image(runtime.sd_ctx, &params, &generated);
        results.adopt(generated, runtime.ctx_params->lora_model_dir);
        if (!generated_ok) {
            error_message = "generate_image failed";
            return false;
        }
        if (job.cancel_requested.load()) {
            return false;
        }
        if (!results.empty() && !apply_server_adetailer(runtime, job.img_gen, params, results)) {
            error_message = "ADetailer failed";
            return false;
        }
    }

    const int num_results = results.count();
    if (job.cancel_requested.load() || num_results <= 0) {
        error_message = "generate_image returned no results";
        return false;
    }

    EncodedImageFormat encoded_format = EncodedImageFormat::PNG;
    if (job.img_gen.output_format == "jpeg") {
        encoded_format = EncodedImageFormat::JPEG;
    } else if (job.img_gen.output_format == "webp") {
        encoded_format = EncodedImageFormat::WEBP;
    }

    for (int i = 0; i < num_results; ++i) {
        if (results[i].data == nullptr) {
            continue;
        }

        const std::string metadata = job.img_gen.gen_params.embed_image_metadata
                                         ? results.generation_info(i)
                                         : "";
        auto image_bytes           = encode_image_to_vector(encoded_format,
                                                            results[i].data,
                                                            results[i].width,
                                                            results[i].height,
                                                            results[i].channel,
                                                            metadata,
                                                            job.img_gen.output_compression);
        if (image_bytes.empty()) {
            continue;
        }
        output_images.push_back(base64_encode(image_bytes));
        output_info.push_back(json::parse(results.generation_info(i)));
    }

    if (output_images.empty()) {
        error_message = "generate_image returned empty encoded outputs";
        return false;
    }

    return true;
}

bool execute_vid_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::string& output_media_b64,
                         std::string& output_media_mime_type,
                         int& output_frame_count,
                         int& output_fps,
                         json& output_info,
                         std::string& error_message) {
    sd_vid_gen_params_t params = job.vid_gen.to_sd_vid_gen_params_t();
    std::string str_params;

    SDImageVec results;
    int num_results             = 0;
    std::unique_ptr<sd_audio_t, decltype(&free_sd_audio)> generated_audio(nullptr, &free_sd_audio);

    {
        std::lock_guard<std::mutex> lock(*runtime.sd_ctx_mutex);
        if (job.cancel_requested.load()) {
            return false;
        }
        JobCancellationMonitor cancellation(runtime.sd_ctx, job);
        PreviewCallbackGuard preview(job);
        sd_video_generation_result_t* generated = nullptr;
        const bool generated_ok = generate_video(runtime.sd_ctx, &params, &generated);
        std::unique_ptr<sd_video_generation_result_t, decltype(&free_sd_video_generation_result)>
            native_result(generated, &free_sd_video_generation_result);
        if (generated_ok && native_result) {
            output_fps = native_result->fps;
            generated_audio.reset(native_result->audio);
            native_result->audio = nullptr;
            results.adopt_video(native_result.release(), runtime.ctx_params->lora_model_dir);
            output_info = json::parse(results.generation_info(0));
            if (job.vid_gen.gen_params.embed_image_metadata)
                str_params = results.generation_info(0);
        }
    }

    num_results = results.count();
    if (job.cancel_requested.load() || num_results <= 0) {
        error_message = "generate_video returned no results";
        return false;
    }

    std::vector<uint8_t> video_bytes = create_video_from_sd_images_to_vector(job.vid_gen.output_format,
                                                                             results.data(),
                                                                             num_results,
                                                                             output_fps,
                                                                             job.vid_gen.output_compression,
                                                                             generated_audio.get(),
                                                                             str_params);
    if (video_bytes.empty()) {
        error_message = "failed to encode generated video container";
        return false;
    }

    output_media_b64       = base64_encode(video_bytes);
    output_media_mime_type = video_mime_type(job.vid_gen.output_format);
    output_frame_count     = num_results;
    return true;
}

void async_job_worker(ServerRuntime& runtime) {
    AsyncJobManager& manager = *runtime.async_job_manager;

    while (true) {
        std::shared_ptr<AsyncGenerationJob> job;
        {
            std::unique_lock<std::mutex> lock(manager.mutex);
            manager.cv.wait(lock, [&]() { return manager.stop || !manager.queue.empty(); });

            if (manager.stop && manager.queue.empty()) {
                break;
            }

            purge_expired_jobs(manager);
            if (manager.queue.empty()) {
                continue;
            }

            job = start_next_async_job(manager);
            if (!job)
                continue;
        }

        std::vector<std::string> output_images;
        json output_info;
        std::string output_media_b64;
        std::string output_media_mime_type;
        int output_frame_count = 0;
        int output_fps         = 0;
        std::string error_message;
        json workflow_output;
        bool ok = false;

        try {
            if (job->kind == AsyncJobKind::ImgGen) {
                ok = execute_img_gen_job(runtime, *job, output_images, output_info, error_message);
            } else if (job->kind == AsyncJobKind::VidGen) {
                ok = execute_vid_gen_job(runtime,
                                         *job,
                                         output_media_b64,
                                         output_media_mime_type,
                                         output_frame_count,
                                         output_fps,
                                         output_info,
                                         error_message);
            } else if (job->kind == AsyncJobKind::Workflow) {
                ok = execute_workflow_job(runtime, *job, workflow_output, error_message);
            } else {
                error_message = "unsupported job kind";
            }
        } catch (const std::exception& e) {
            error_message = e.what();
        } catch (...) {
            error_message = "unknown generation exception";
        }

        AsyncJobCompletion completion;
        completion.success = ok;
        completion.result_images_b64 = std::move(output_images);
        completion.result_generation_info = std::move(output_info);
        completion.result_media_b64 = std::move(output_media_b64);
        completion.result_media_mime_type = std::move(output_media_mime_type);
        completion.result_frame_count = output_frame_count;
        completion.result_fps = output_fps;
        completion.result_workflow = std::move(workflow_output);
        completion.error_message = std::move(error_message);

        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            const auto it = manager.jobs.find(job->id);
            if (it == manager.jobs.end())
                continue;
            finish_async_job(manager, *job, std::move(completion));
            purge_expired_jobs(manager);
        }
    }
}
