#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>


#include "runtime.h"
#include "workflow_v1.h"

enum class AsyncJobKind {
    ImgGen,
    VidGen,
    Workflow,
};

enum class AsyncJobStatus {
    Queued,
    Generating,
    Completed,
    Failed,
    Cancelled,
};

const char* async_job_kind_name(AsyncJobKind kind);
const char* async_job_status_name(AsyncJobStatus status);

struct AsyncGenerationJob {
    std::string id;
    AsyncJobKind kind     = AsyncJobKind::ImgGen;
    AsyncJobStatus status = AsyncJobStatus::Queued;
    std::atomic<bool> cancel_requested{false};
    int64_t created_at   = unix_timestamp_now_ms();
    int64_t started_at   = 0;
    int64_t completed_at = 0;
    ImgGenJobRequest img_gen;
    VidGenJobRequest vid_gen;
    WorkflowV1 workflow;
    std::vector<std::string> workflow_node_states; // protected by manager.mutex
    json result_workflow;
    std::vector<std::string> result_images_b64;
    json result_generation_info;
    std::string result_media_b64;
    std::string result_media_mime_type;
    int result_frame_count = 0;
    int result_fps         = 0;
    std::string error_code;
    std::string error_message;
    mutable std::mutex preview_mutex;
    std::string preview_b64;
    int preview_pass          = 0;
    int preview_step          = 0;
    int preview_total_steps   = 0;
};

struct AsyncJobManager {
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, std::shared_ptr<AsyncGenerationJob>> jobs;
    std::unordered_map<std::string, int64_t> expired_jobs;
    std::deque<std::string> queue;
    // Must be initialized from ServerModels.instance_id before worker startup.
    // Task IDs are process-scoped identifiers, never authentication tokens.
    std::string instance_id;
    uint64_t next_id              = 0;
    bool stop                     = false;
    size_t max_pending_jobs       = 64;
    // Bound *all* retained job records (queued, generating and terminal),
    // independently of pending capacity and the 4-workflow retention limit.
    size_t max_retained_jobs      = 256;
    int64_t completed_ttl_seconds = 600;
    int64_t failed_ttl_seconds    = 600;
};

// Owned result payload, prepared outside the manager lock by the one native
// worker. Tests may submit deterministic completion/failure payloads directly
// to the *same* terminal state machine; no fake inference backend is installed.
// Admission is a single ordered queue/map commit. The receipt body is
// materialized before publication and never reads mutable job status after
// wakeup; all 3 native submit routes must use this one owner.
enum class AsyncJobAdmissionStatus {
    Accepted,
    Stopping,
    QueueFull,
    RetainedFull,
    WorkflowFull,
};

struct AsyncJobAdmission {
    AsyncJobAdmissionStatus status = AsyncJobAdmissionStatus::Stopping;
    std::string receipt_json;
};

AsyncJobAdmission admit_async_job(AsyncJobManager& manager,
                                  const std::shared_ptr<AsyncGenerationJob>& job);

struct AsyncJobCompletion {
    bool success = false;
    std::vector<std::string> result_images_b64;
    json result_generation_info;
    std::string result_media_b64;
    std::string result_media_mime_type;
    int result_frame_count = 0;
    int result_fps = 0;
    json result_workflow;
    std::string error_message;
};

void purge_expired_jobs(AsyncJobManager& manager);
size_t count_pending_jobs(const AsyncJobManager& manager);
std::string make_async_job_id(AsyncJobManager& manager);
bool cancel_queued_job(AsyncJobManager& manager, AsyncGenerationJob& job);
// These functions require manager.mutex to be held by the caller.
std::shared_ptr<AsyncGenerationJob> start_next_async_job(AsyncJobManager& manager);
bool finish_async_job(AsyncJobManager& manager, AsyncGenerationJob& job,
                      AsyncJobCompletion&& completion);
json make_async_job_json(const AsyncJobManager& manager, const AsyncGenerationJob& job);
// Snapshot only a terminal immutable job. May be called without manager.mutex
// while holding a shared_ptr lease obtained under that mutex.
json make_async_terminal_job_json(const AsyncGenerationJob& job);
bool execute_img_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::vector<std::string>& output_images,
                         json& output_info,
                         std::string& error_message);
bool execute_vid_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::string& output_media_b64,
                         std::string& output_media_mime_type,
                         int& output_frame_count,
                         int& output_fps,
                         json& output_info,
                         std::string& error_message);
void async_job_worker(ServerRuntime& runtime);
// Stop admission, cancel queued jobs and request running cancellation before join.
void stop_async_job_worker(AsyncJobManager& manager);
