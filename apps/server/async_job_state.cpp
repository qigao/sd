// Async job status/queue ownership, independent of model execution.
#include "async_jobs.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

int64_t unix_timestamp_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* async_job_kind_name(AsyncJobKind kind) {
    switch (kind) {
        case AsyncJobKind::ImgGen:
            return "img_gen";
        case AsyncJobKind::VidGen:
            return "vid_gen";
        case AsyncJobKind::Workflow:
            return "workflow";
        default:
            return "img_gen";
    }
}

const char* async_job_status_name(AsyncJobStatus status) {
    switch (status) {
        case AsyncJobStatus::Queued:
            return "queued";
        case AsyncJobStatus::Generating:
            return "generating";
        case AsyncJobStatus::Completed:
            return "completed";
        case AsyncJobStatus::Failed:
            return "failed";
        case AsyncJobStatus::Cancelled:
            return "cancelled";
        default:
            return "failed";
    }
}

void purge_expired_jobs(AsyncJobManager& manager) {
    const int64_t now = unix_timestamp_now_ms();

    for (auto it = manager.expired_jobs.begin(); it != manager.expired_jobs.end();) {
        if (it->second <= now) {
            it = manager.expired_jobs.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = manager.jobs.begin(); it != manager.jobs.end();) {
        const auto& job = it->second;
        if (job->completed_at == 0) {
            ++it;
            continue;
        }

        int64_t ttl_seconds = job->status == AsyncJobStatus::Completed
                                  ? manager.completed_ttl_seconds
                                  : manager.failed_ttl_seconds;
        if (now - job->completed_at >= ttl_seconds * 1000) {
            manager.expired_jobs[job->id] = now + std::max<int64_t>(ttl_seconds, 60) * 1000;
            it                            = manager.jobs.erase(it);
        } else {
            ++it;
        }
    }
}

size_t count_pending_jobs(const AsyncJobManager& manager) {
    size_t pending = 0;
    for (const auto& entry : manager.jobs) {
        if (entry.second->status == AsyncJobStatus::Queued ||
            entry.second->status == AsyncJobStatus::Generating) {
            ++pending;
        }
    }
    return pending;
}

std::string make_async_job_id(AsyncJobManager& manager) {
    // Same runtime nonce as /sdcpp/v1/models/status. An unrelated restarted
    // process must never alias a stale job ID even when timestamp and ordinal
    // collide. This identifier does not convey job authorization.
    const auto& instance = manager.instance_id;
    if (instance.size() != 32 ||
        !std::all_of(instance.begin(), instance.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        throw std::logic_error("async job owner lacks a valid process instance");
    }
    std::ostringstream oss;
    oss << "job_" << instance << "_" << std::hex << unix_timestamp_now_ms()
        << "_" << std::setw(8) << std::setfill('0') << manager.next_id++;
    return oss.str();
}

AsyncJobAdmission admit_async_job(AsyncJobManager& manager,
                                  const std::shared_ptr<AsyncGenerationJob>& job) {
    if (!job)
        throw std::invalid_argument("async job admission requires a job owner");

    AsyncJobAdmission admission;
    {
        std::unique_lock<std::mutex> lock(manager.mutex);
        // Validate fields under the same lock as the worker's status writes;
        // an accidental repeat admission must never race with the running job.
        if (job->status != AsyncJobStatus::Queued || !job->id.empty() ||
            job->cancel_requested.load()) {
            throw std::invalid_argument("async job admission requires a fresh, uncancelled job");
        }
        purge_expired_jobs(manager);
        if (manager.stop) {
            admission.status = AsyncJobAdmissionStatus::Stopping;
            return admission;
        }
        if (count_pending_jobs(manager) >= manager.max_pending_jobs) {
            admission.status = AsyncJobAdmissionStatus::QueueFull;
            return admission;
        }
        if (job->kind == AsyncJobKind::Workflow) {
            size_t retained_workflows = 0;
            for (const auto& existing : manager.jobs) {
                if (existing.second->kind == AsyncJobKind::Workflow)
                    ++retained_workflows;
            }
            if (retained_workflows >= 4) {
                admission.status = AsyncJobAdmissionStatus::WorkflowFull;
                return admission;
            }
        }

        // This JSON is an immutable receipt of admission (not the mutable
        // worker state). Format it before publishing any entry to the worker.
        job->id = make_async_job_id(manager);
        admission.receipt_json = json({
            {"id", job->id},
            {"kind", async_job_kind_name(job->kind)},
            {"status", "queued"},
            {"created", job->created_at},
            {"poll_url", "/sdcpp/v1/jobs/" + job->id},
        }).dump();

        const auto insertion = manager.jobs.emplace(job->id, job);
        if (!insertion.second) {
            throw std::logic_error("duplicate async job id");
        }
        try {
            manager.queue.push_back(job->id);
        } catch (...) {
            manager.jobs.erase(insertion.first);
            throw;
        }
        admission.status = AsyncJobAdmissionStatus::Accepted;
    }
    // Never let the worker race with receipt preparation or registry publish.
    manager.cv.notify_one();
    return admission;
}

namespace { void release_generation_inputs(AsyncGenerationJob& job); }

bool cancel_queued_job(AsyncJobManager& manager, AsyncGenerationJob& job) {
    auto new_end = std::remove(manager.queue.begin(), manager.queue.end(), job.id);
    if (new_end == manager.queue.end()) {
        return false;
    }

    manager.queue.erase(new_end, manager.queue.end());
    job.cancel_requested.store(true);
    job.status       = AsyncJobStatus::Cancelled;
    job.completed_at = unix_timestamp_now_ms();
    job.result_images_b64.clear();
    job.result_generation_info = nullptr;
    job.result_media_b64.clear();
    job.result_media_mime_type.clear();
    job.result_frame_count = 0;
    job.result_fps         = 0;
    job.error_code         = "cancelled";
    job.error_message      = "job cancelled by client";
    job.result_workflow = nullptr;
    for (auto& state : job.workflow_node_states) state = "skipped";
    {
        std::lock_guard<std::mutex> preview_lock(job.preview_mutex);
        job.preview_b64.clear();
    }
    release_generation_inputs(job);
    return true;
}

static json make_async_job_json_impl(const AsyncJobManager* manager, const AsyncGenerationJob& job) {
    json result;
    result["id"]               = job.id;
    result["kind"]             = async_job_kind_name(job.kind);
    result["status"]           = async_job_status_name(job.status);
    result["cancel_requested"] = job.cancel_requested.load();
    result["created"]          = job.created_at;
    result["started"]          = job.started_at == 0 ? json(nullptr) : json(job.started_at);
    result["completed"]        = job.completed_at == 0 ? json(nullptr) : json(job.completed_at);
    result["queue_position"]   = 0;

    if (job.status == AsyncJobStatus::Queued) {
        if (manager == nullptr) {
            throw std::logic_error("queued job requires manager-locked snapshot");
        }
        size_t position = 1;
        for (const auto& queued_id : manager->queue) {
            if (queued_id == job.id) {
                result["queue_position"] = position;
                break;
            }
            ++position;
        }
    }

    if (job.kind == AsyncJobKind::Workflow) {
        result["nodes"] = json::array();
        for (size_t i = 0; i < job.workflow.nodes.size(); ++i) {
            result["nodes"].push_back({{"id", job.workflow.nodes[i].id},
                                       {"status", job.workflow_node_states[i]}});
        }
    }
    if (job.status == AsyncJobStatus::Completed) {
        if (job.kind == AsyncJobKind::Workflow) {
            result["result"] = job.result_workflow;
        } else if (job.kind == AsyncJobKind::VidGen) {
            result["result"] = {
                {"output_format", job.vid_gen.output_format},
                {"mime_type", job.result_media_mime_type},
                {"fps", job.result_fps},
                {"frame_count", job.result_frame_count},
                {"b64_json", job.result_media_b64},
                {"generation_info", job.result_generation_info},
            };
        } else {
            json images = json::array();
            for (size_t i = 0; i < job.result_images_b64.size(); ++i) {
                images.push_back({{"index", i}, {"b64_json", job.result_images_b64[i]}, {"generation_info", job.result_generation_info.at(i)}});
            }
            result["result"] = {
                {"output_format", job.img_gen.output_format},
                {"images", images},
            };
        }
        result["error"] = nullptr;
    } else if (job.status == AsyncJobStatus::Failed ||
               job.status == AsyncJobStatus::Cancelled) {
        result["result"] = nullptr;
        result["error"]  = {
            {"code",
             job.error_code.empty()
                 ? (job.status == AsyncJobStatus::Cancelled ? "cancelled" : "generation_failed")
                 : job.error_code},
            {"message", job.error_message},
        };
    } else {
        result["result"] = nullptr;
        result["error"]  = nullptr;
    }

    if (job.status == AsyncJobStatus::Generating) {
        std::lock_guard<std::mutex> lock(job.preview_mutex);
        if (!job.preview_b64.empty()) {
            result["preview"] = {
                {"pass", job.preview_pass},
                {"step", job.preview_step},
                {"total_steps", job.preview_total_steps},
                {"b64_json", job.preview_b64},
            };
        }
    }

    return result;
}

json make_async_job_json(const AsyncJobManager& manager, const AsyncGenerationJob& job) {
    // Caller holds manager.mutex for queued/running snapshots, including preview.
    return make_async_job_json_impl(&manager, job);
}

json make_async_terminal_job_json(const AsyncGenerationJob& job) {
    // Only terminal jobs are immutable after the shared manager commit point.
    // A shared_ptr reader lease keeps the payload alive even if the TTL
    // sweeper erases the registry entry while HTTP serialization is running.
    if (job.status == AsyncJobStatus::Queued || job.status == AsyncJobStatus::Generating)
        throw std::logic_error("non-terminal job requires manager-locked snapshot");
    return make_async_job_json_impl(nullptr, job);
}

std::shared_ptr<AsyncGenerationJob> start_next_async_job(AsyncJobManager& manager) {
    // Caller holds manager.mutex. The queue and job status share one owner.
    while (!manager.queue.empty()) {
        const std::string id = std::move(manager.queue.front());
        manager.queue.pop_front();
        auto it = manager.jobs.find(id);
        if (it == manager.jobs.end() || it->second->status != AsyncJobStatus::Queued)
            continue;
        auto job = it->second;
        job->status = AsyncJobStatus::Generating;
        job->started_at = unix_timestamp_now_ms();
        return job;
    }
    return {};
}

namespace {
// Terminal publication runs under manager.mutex; it must not allocate merely
// to release request resources. All sd.cpp callback and parameter borrows have
// unwound before this point. The output format is the only request property
// still needed by image/video result serialization.
void retire_text(std::string& text) noexcept {
    std::string empty;
    text.swap(empty);
}

template <typename T>
void retire_vector(std::vector<T>& items) noexcept {
    std::vector<T> empty;
    items.swap(empty);
}

void retire_generation_params(SDGenerationParams& params) noexcept {
    // Retire raw views before destroying their owning image/audio buffers.
    retire_vector(params.lora_vec);
    retire_vector(params.ref_image_views);
    retire_vector(params.ref_video_frame_views);
    retire_vector(params.ref_video_views);
    retire_vector(params.ref_audio_views);
    retire_vector(params.pm_id_image_views);
    retire_vector(params.control_frame_views);

    params.init_image.reset();
    params.end_image.reset();
    params.mask_image.reset();
    params.control_image.reset();
    params.ip_adapter_image.reset();
    retire_vector(params.ref_images);
    retire_vector(params.ref_videos);
    retire_vector(params.ref_video_audios);
    retire_vector(params.ref_audios);
    retire_vector(params.pm_id_images);
    retire_vector(params.control_frames);

    retire_vector(params.ref_image_paths);
    retire_vector(params.ref_video_paths);
    retire_vector(params.ref_video_audio_paths);
    retire_vector(params.ref_audio_paths);
    retire_vector(params.skip_layers);
    retire_vector(params.high_noise_skip_layers);
    retire_vector(params.custom_sigmas);
    retire_vector(params.hires_custom_sigmas);

    params.lora_map.clear();
    params.high_noise_lora_map.clear();

    for (std::string* value : {
        &params.prompt, &params.negative_prompt, &params.ad_model_path,
        &params.ad_prompt, &params.ad_negative_prompt, &params.extra_ad_args,
        &params.init_image_path, &params.end_image_path, &params.mask_image_path,
        &params.control_image_path, &params.ip_adapter_image_path,
        &params.control_video_path, &params.extra_sample_args,
        &params.high_noise_extra_sample_args, &params.cache_mode,
        &params.cache_option, &params.scm_mask, &params.extra_tiling_args,
        &params.ref_image_args, &params.image_preprocess, &params.pm_id_images_dir,
        &params.pm_id_embed_path, &params.pulid_id_embedding_path,
        &params.hires_upscaler, &params.hires_upscaler_model_path,
        &params.prompt_with_lora
    }) {
        retire_text(*value);
    }
    // Do not leave pointer views dangling inside a retained terminal object.
    params.sample_params.guidance.slg.layers = nullptr;
    params.sample_params.guidance.slg.layer_count = 0;
    params.high_noise_sample_params.guidance.slg.layers = nullptr;
    params.high_noise_sample_params.guidance.slg.layer_count = 0;
    params.sample_params.custom_sigmas = nullptr;
    params.sample_params.custom_sigmas_count = 0;
    params.high_noise_sample_params.custom_sigmas = nullptr;
    params.high_noise_sample_params.custom_sigmas_count = 0;
    params.sample_params.extra_sample_args = nullptr;
    params.high_noise_sample_params.extra_sample_args = nullptr;
    params.vae_tiling_params.extra_tiling_args = nullptr;
    params.cache_params.scm_mask = nullptr;
}

void release_generation_inputs(AsyncGenerationJob& job) noexcept {
    if (job.kind == AsyncJobKind::Workflow) {
        // Terminal status exposes only node IDs, node states and result JSON.
        // Free the original prompts, parameters, model binding and DAG edges
        // rather than retaining the entire request for the 10-minute job TTL.
        for (auto& node : job.workflow.nodes) {
            node.parameters.clear();
            retire_vector(node.dependencies);
        }
        retire_vector(job.workflow.order);
        retire_vector(job.workflow.outputs);
        retire_text(job.workflow.model_recipe);
        retire_text(job.workflow.model_instance);
    }
    retire_generation_params(job.img_gen.gen_params);
    retire_generation_params(job.vid_gen.gen_params);
    retire_vector(job.img_gen.adetailer_passes);
    retire_text(job.img_gen.preview_mode);
    retire_text(job.vid_gen.preview_mode);
}

void clear_outputs(AsyncGenerationJob& job) {
    job.result_images_b64.clear();
    job.result_generation_info = nullptr;
    job.result_media_b64.clear();
    job.result_media_mime_type.clear();
    job.result_frame_count = 0;
    job.result_fps = 0;
    job.result_workflow = nullptr;
}
} // namespace

bool finish_async_job(AsyncJobManager& manager, AsyncGenerationJob& job,
                      AsyncJobCompletion&& completion) {
    // Caller holds manager.mutex; a successful completion/cancellation race is
    // ordered against the HTTP cancel route by the same lock. No later write is
    // permitted once a terminal status has been published.
    (void)manager;
    if (job.status != AsyncJobStatus::Generating)
        return false;

    // Materialize the fallback error before publishing any terminal status.
    if (!completion.success && completion.error_message.empty())
        completion.error_message = "unknown generation error";

    if (job.cancel_requested.load()) {
        job.error_code = "cancelled";
        job.error_message = "job cancelled by client";
        clear_outputs(job);
        job.status = AsyncJobStatus::Cancelled;
    } else if (completion.success) {
        job.result_images_b64 = std::move(completion.result_images_b64);
        job.result_generation_info = std::move(completion.result_generation_info);
        job.result_media_b64 = std::move(completion.result_media_b64);
        job.result_media_mime_type = std::move(completion.result_media_mime_type);
        job.result_frame_count = completion.result_frame_count;
        job.result_fps = completion.result_fps;
        job.result_workflow = std::move(completion.result_workflow);
        job.error_code.clear();
        job.error_message.clear();
        job.status = AsyncJobStatus::Completed;
    } else {
        job.error_code = "generation_failed";
        job.error_message = std::move(completion.error_message);
        clear_outputs(job);
        job.status = AsyncJobStatus::Failed;
    }
    if (job.kind == AsyncJobKind::Workflow && job.status != AsyncJobStatus::Completed) {
        for (auto& state : job.workflow_node_states) {
            if (state == "running")
                state = job.status == AsyncJobStatus::Cancelled ? "cancelled" : "failed";
            else if (state == "queued")
                state = "skipped";
        }
    }
    // Callback owner and generation monitor are already unwound when the
    // worker reaches this function. Preview is never needed in a terminal job.
    {
        std::lock_guard<std::mutex> preview_lock(job.preview_mutex);
        job.preview_b64.clear();
    }
    release_generation_inputs(job);
    job.completed_at = unix_timestamp_now_ms();
    return true;
}

void stop_async_job_worker(AsyncJobManager& manager) {
    {
        std::lock_guard<std::mutex> lock(manager.mutex);
        manager.stop = true;
        // The worker must not drain newly orphaned generations during shutdown.
        // A running callback/ctx stays alive until the worker has been joined.
        while (!manager.queue.empty()) {
            const auto it = manager.jobs.find(manager.queue.front());
            if (it != manager.jobs.end() && it->second->status == AsyncJobStatus::Queued)
                cancel_queued_job(manager, *it->second);
            else
                manager.queue.pop_front();
        }
        for (const auto& entry : manager.jobs) {
            if (entry.second->status == AsyncJobStatus::Generating)
                entry.second->cancel_requested.store(true);
        }
    }
    manager.cv.notify_all();
}

