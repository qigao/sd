#include "async_jobs.h"
#include "preview_limits.h"
#include "tinytest.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A test harness uses a controlled non-secret process identity to exercise
// the same real ID generator and admission owner without model weights.
struct TestAsyncJobManager : AsyncJobManager {
    TestAsyncJobManager() {
        instance_id = "0123456789abcdef0123456789abcdef";
    }
};

// These tests run the *production* queue/terminal publication code without
// replacing the sd.cpp inference provider. Simulated results are only injected
// at the owned worker completion boundary. They do not prove real weights.
static std::shared_ptr<AsyncGenerationJob> enqueue(AsyncJobManager& manager,
                                                    AsyncJobKind kind) {
    auto job = std::make_shared<AsyncGenerationJob>();
    job->kind = kind;
    job->id = make_async_job_id(manager);
    if (kind == AsyncJobKind::Workflow) {
        job->workflow.nodes.resize(2);
        job->workflow.nodes[0].id = "first";
        job->workflow.nodes[1].id = "second";
        job->workflow_node_states = {"queued", "queued"};
        job->workflow.outputs = {1};
    }
    manager.jobs.emplace(job->id, job);
    manager.queue.push_back(job->id);
    return job;
}

static AsyncJobCompletion successful_output(AsyncJobKind kind = AsyncJobKind::Workflow) {
    AsyncJobCompletion output;
    output.success = true;
    if (kind == AsyncJobKind::ImgGen) {
        output.result_images_b64 = {"encoded"};
        output.result_generation_info = json::array({json::object()});
    } else if (kind == AsyncJobKind::VidGen) {
        output.result_media_b64 = "encoded";
        output.result_media_mime_type = "video/webm";
        output.result_fps = 16;
        output.result_frame_count = 1;
        output.result_generation_info = json::object();
    } else {
        output.result_workflow = {
            {"version", 1},
            {"model", json::object()},
            {"outputs", json::array({
                {{"node_id", "second"},
                 {"images", json::array({
                     {{"index", 0}, {"b64_json", "encoded"},
                      {"generation_info", json::object()}}
                 })}}
            })}
        };
    }
    return output;
}

spec("AsyncJobManager native terminal ownership and fault injection")
{
    it("publishes a failed native result without leaking partial media or completing dependents")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::Workflow);
        check_true(start_next_async_job(manager) == job);
        job->workflow_node_states = {"completed", "running"};
        job->img_gen.gen_params.prompt = "ephemeral decoded request";
        AsyncJobCompletion outcome = successful_output();
        outcome.success = false;
        outcome.error_message = "injected native generation failure";
        check_true(finish_async_job(manager, *job, std::move(outcome)));
        check_true(job->status == AsyncJobStatus::Failed);
        check_equal(job->workflow_node_states[0], std::string("completed"));
        check_equal(job->workflow_node_states[1], std::string("failed"));
        check_true(job->result_workflow.is_null());
        check_true(job->result_images_b64.empty());
        check_true(job->img_gen.gen_params.prompt.empty());
        const json snapshot = make_async_job_json(manager, *job);
        check_true(snapshot["result"].is_null());
        check_equal(snapshot["error"]["code"].get<std::string>(), std::string("generation_failed"));
        check_equal(snapshot["error"]["message"].get<std::string>(),
                    std::string("injected native generation failure"));
        check_equal(count_pending_jobs(manager), static_cast<size_t>(0));
        check_false(finish_async_job(manager, *job, successful_output()));
        check_true(job->status == AsyncJobStatus::Failed);
    }

    it("drops owned inputs, outputs and preview at a queued-cancellation terminal")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::Workflow);
        job->img_gen.gen_params.prompt = std::string(2048, 'x');
        job->preview_b64 = std::string(2048, 'a');
        check_true(cancel_queued_job(manager, *job));
        check_true(manager.queue.empty());
        check_true(job->cancel_requested.load());
        check_true(job->status == AsyncJobStatus::Cancelled);
        check_true(job->result_workflow.is_null());
        check_true(job->img_gen.gen_params.prompt.empty());
        check_true(job->preview_b64.empty());
        check_true(job->workflow_node_states[0] == "skipped");
        check_true(job->workflow_node_states[1] == "skipped");
        check_true(make_async_job_json(manager, *job)["result"].is_null());
        check_false(cancel_queued_job(manager, *job));
        check_equal(count_pending_jobs(manager), static_cast<size_t>(0));
    }

    it("lets cancel win before native success publication without retaining any result")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::Workflow);
        check_true(start_next_async_job(manager) == job);
        job->workflow_node_states = {"running", "queued"};
        job->cancel_requested.store(true);
        check_true(finish_async_job(manager, *job, successful_output()));
        check_true(job->status == AsyncJobStatus::Cancelled);
        check_true(job->result_images_b64.empty());
        check_true(job->result_workflow.is_null());
        check_true(job->workflow_node_states[0] == "cancelled");
        check_true(job->workflow_node_states[1] == "skipped");
        const json snapshot = make_async_job_json(manager, *job);
        check_true(snapshot["result"].is_null());
        check_equal(snapshot["error"]["code"].get<std::string>(), std::string("cancelled"));
    }

    it("preserves an already published success terminal after cancel loses")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::Workflow);
        check_true(start_next_async_job(manager) == job);
        job->workflow_node_states = {"completed", "completed"};
        check_true(finish_async_job(manager, *job, successful_output()));
        check_true(job->status == AsyncJobStatus::Completed);
        check_false(cancel_queued_job(manager, *job));
        check_false(finish_async_job(manager, *job, AsyncJobCompletion{}));
        check_true(job->status == AsyncJobStatus::Completed);
        const json snapshot = make_async_job_json(manager, *job);
        check_true(snapshot["error"].is_null());
        check_equal(snapshot["result"]["version"].get<int>(), 1);
        check_true(job->completed_at > 0);
    }

    it("cancels queued work at shutdown without freeing the running native owner prematurely")
    {
        TestAsyncJobManager manager;
        std::shared_ptr<AsyncGenerationJob> running, waiting;
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            running = enqueue(manager, AsyncJobKind::Workflow);
            waiting = enqueue(manager, AsyncJobKind::ImgGen);
            waiting->img_gen.gen_params.prompt = "large queued input";
            check_true(start_next_async_job(manager) == running);
        }
        stop_async_job_worker(manager);
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            check_true(manager.stop);
            check_true(manager.queue.empty());
            check_true(running->status == AsyncJobStatus::Generating);
            check_true(running->cancel_requested.load());
            check_true(waiting->status == AsyncJobStatus::Cancelled);
            check_true(waiting->img_gen.gen_params.prompt.empty());
            check_true(start_next_async_job(manager) == nullptr);
            check_true(finish_async_job(manager, *running, successful_output()));
            check_true(running->status == AsyncJobStatus::Cancelled);
        }
    }

    it("serializes cancellation and success under the same owner mutex")
    {
        for (int iteration = 0; iteration < 64; ++iteration) {
            TestAsyncJobManager manager;
            std::shared_ptr<AsyncGenerationJob> job;
            {
                std::lock_guard<std::mutex> lock(manager.mutex);
                job = enqueue(manager, AsyncJobKind::Workflow);
                check_true(start_next_async_job(manager) == job);
                job->workflow_node_states = {"running", "queued"};
            }
            std::atomic<int> arrived{0};
            std::thread cancel([&] {
                arrived.fetch_add(1);
                while (arrived.load() < 2) std::this_thread::yield();
                std::lock_guard<std::mutex> lock(manager.mutex);
                // Exactly the HTTP cancel route's terminal-state guard.
                if (job->status == AsyncJobStatus::Generating)
                    job->cancel_requested.store(true);
            });
            std::thread complete([&] {
                arrived.fetch_add(1);
                while (arrived.load() < 2) std::this_thread::yield();
                std::lock_guard<std::mutex> lock(manager.mutex);
                // The real Workflow scheduler marks its nodes completed before
                // publishing a success result. Race only against HTTP cancel.
                job->workflow_node_states = {"completed", "completed"};
                finish_async_job(manager, *job, successful_output());
            });
            cancel.join();
            complete.join();
            {
                std::lock_guard<std::mutex> lock(manager.mutex);
                check_true(job->status == AsyncJobStatus::Completed ||
                           job->status == AsyncJobStatus::Cancelled);
                const json result = make_async_job_json(manager, *job);
                if (job->status == AsyncJobStatus::Completed) {
                    check_true(result["error"].is_null());
                    check_true(!result["result"].is_null());
                    check_false(job->cancel_requested.load());
                } else {
                    check_true(result["result"].is_null());
                    check_true(job->result_images_b64.empty());
                }
            }
        }
    }

    it("expires terminal payloads, preserves pending owners and then expires tombstones")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto finished = enqueue(manager, AsyncJobKind::Workflow);
        auto pending = enqueue(manager, AsyncJobKind::Workflow);
        // Simulate a completed job whose TTL elapsed.
        finished->status = AsyncJobStatus::Failed;
        finished->completed_at = unix_timestamp_now_ms() - (manager.failed_ttl_seconds + 2) * 1000;
        purge_expired_jobs(manager);
        check_true(manager.jobs.find(finished->id) == manager.jobs.end());
        check_true(manager.expired_jobs.find(finished->id) != manager.expired_jobs.end());
        check_true(manager.jobs.find(pending->id) != manager.jobs.end());
        check_true(count_pending_jobs(manager) == 1);
        manager.expired_jobs[finished->id] = unix_timestamp_now_ms() - 1;
        purge_expired_jobs(manager);
        check_true(manager.expired_jobs.find(finished->id) == manager.expired_jobs.end());
    }
    it("commits a single immutable receipt before the worker can advance")
    {
        TestAsyncJobManager manager;
        auto job = std::make_shared<AsyncGenerationJob>();
        job->kind = AsyncJobKind::VidGen;
        const AsyncJobAdmission receipt = admit_async_job(manager, job);
        check_true(receipt.status == AsyncJobAdmissionStatus::Accepted);
        const json published = json::parse(receipt.receipt_json);
        check_equal(published["id"].get<std::string>(), job->id);
        check_equal(published["kind"].get<std::string>(), std::string("vid_gen"));
        check_equal(published["status"].get<std::string>(), std::string("queued"));
        check_equal(published["poll_url"].get<std::string>(),
                    std::string("/sdcpp/v1/jobs/") + job->id);
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            check_true(start_next_async_job(manager) == job);
            check_true(finish_async_job(manager, *job, successful_output(AsyncJobKind::VidGen)));
            check_true(job->status == AsyncJobStatus::Completed);
        }
        // The caller's 202 body can never change to 'generating' or 'completed'.
        check_equal(json::parse(receipt.receipt_json)["status"].get<std::string>(),
                    std::string("queued"));
    }

    it("rejects stopped and full admissions without publishing a job")
    {
        TestAsyncJobManager manager;
        manager.max_pending_jobs = 0;
        auto waiting = std::make_shared<AsyncGenerationJob>();
        auto blocked = admit_async_job(manager, waiting);
        check_true(blocked.status == AsyncJobAdmissionStatus::QueueFull);
        check_true(blocked.receipt_json.empty());
        check_true(waiting->id.empty());
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            check_true(manager.jobs.empty());
            check_true(manager.queue.empty());
            manager.max_pending_jobs = 2;
        }
        stop_async_job_worker(manager);
        waiting = std::make_shared<AsyncGenerationJob>();
        blocked = admit_async_job(manager, waiting);
        check_true(blocked.status == AsyncJobAdmissionStatus::Stopping);
        check_true(waiting->id.empty());
        check_true(manager.jobs.empty());
    }

    it("caps retained workflows independently of available pending slots")
    {
        TestAsyncJobManager manager;
        manager.max_pending_jobs = 64;
        std::vector<std::shared_ptr<AsyncGenerationJob>> admitted;
        for (int i = 0; i < 4; ++i) {
            auto job = std::make_shared<AsyncGenerationJob>();
            job->kind = AsyncJobKind::Workflow;
            auto receipt = admit_async_job(manager, job);
            check_true(receipt.status == AsyncJobAdmissionStatus::Accepted);
            admitted.push_back(job);
            std::lock_guard<std::mutex> lock(manager.mutex);
            check_true(cancel_queued_job(manager, *job));
        }
        auto fifth = std::make_shared<AsyncGenerationJob>();
        fifth->kind = AsyncJobKind::Workflow;
        auto rejected = admit_async_job(manager, fifth);
        check_true(rejected.status == AsyncJobAdmissionStatus::WorkflowFull);
        check_true(fifth->id.empty());
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            check_equal(manager.queue.size(), static_cast<size_t>(0));
            check_equal(manager.jobs.size(), static_cast<size_t>(4));
            admitted[0]->completed_at = unix_timestamp_now_ms() -
                (manager.failed_ttl_seconds + 2) * 1000;
        }
        const auto accepted = admit_async_job(manager, fifth);
        check_true(accepted.status == AsyncJobAdmissionStatus::Accepted);
        check_false(fifth->id.empty());
        std::lock_guard<std::mutex> lock(manager.mutex);
        check_true(manager.jobs.find(admitted[0]->id) == manager.jobs.end());
        check_equal(manager.jobs.size(), static_cast<size_t>(4));
    }

    it("admits at most one job under concurrent capacity contention")
    {
        TestAsyncJobManager manager;
        manager.max_pending_jobs = 1;
        constexpr int contenders = 12;
        std::atomic<int> started{0};
        std::atomic<int> accepted{0};
        std::atomic<int> rejected{0};
        std::vector<std::thread> workers;
        workers.reserve(contenders);
        for (int i = 0; i < contenders; ++i) {
            workers.emplace_back([&] {
                auto job = std::make_shared<AsyncGenerationJob>();
                job->kind = AsyncJobKind::ImgGen;
                started.fetch_add(1);
                while (started.load() < contenders) std::this_thread::yield();
                const auto result = admit_async_job(manager, job);
                if (result.status == AsyncJobAdmissionStatus::Accepted)
                    accepted.fetch_add(1);
                else if (result.status == AsyncJobAdmissionStatus::QueueFull)
                    rejected.fetch_add(1);
            });
        }
        for (auto& thread : workers) thread.join();
        check_equal(accepted.load(), 1);
        check_equal(rejected.load(), contenders - 1);
        std::lock_guard<std::mutex> lock(manager.mutex);
        check_equal(manager.jobs.size(), static_cast<size_t>(1));
        check_equal(manager.queue.size(), static_cast<size_t>(1));
        const auto it = manager.jobs.find(manager.queue.front());
        check_true(it != manager.jobs.end());
        if (it != manager.jobs.end())
            check_true(it->second->status == AsyncJobStatus::Queued);
    }

    it("refuses re-admission of an already published job object")
    {
        TestAsyncJobManager manager;
        auto job = std::make_shared<AsyncGenerationJob>();
        const auto first = admit_async_job(manager, job);
        check_true(first.status == AsyncJobAdmissionStatus::Accepted);
        bool rejected = false;
        try {
            (void)admit_async_job(manager, job);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check_true(rejected);
        std::lock_guard<std::mutex> lock(manager.mutex);
        check_equal(manager.jobs.size(), static_cast<size_t>(1));
        check_equal(manager.queue.size(), static_cast<size_t>(1));
    }


    it("retires all Workflow V1 node params and DAG inputs after queued cancellation")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::Workflow);
        job->workflow.model_recipe = "checkpoint-with-private-path";
        job->workflow.model_instance = manager.instance_id;
        job->workflow.order = {0, 1};
        job->workflow.outputs = {1};
        job->workflow.nodes[0].parameters = {
            {"prompt", std::string(2048, 'p')},
            {"negative_prompt", std::string(2048, 'n')},
            {"width", 128}, {"height", 128}, {"steps", 2},
        };
        job->workflow.nodes[1].parameters = {
            {"prompt", std::string(2048, 'q')},
            {"width", 128}, {"height", 128}, {"steps", 2},
        };
        job->workflow.nodes[1].dependencies = {0};
        job->img_gen.gen_params.prompt = std::string(2048, 'w');
        check_true(cancel_queued_job(manager, *job));
        check_true(job->status == AsyncJobStatus::Cancelled);
        check_true(job->workflow.model_recipe.empty());
        check_true(job->workflow.model_instance.empty());
        check_true(job->workflow.order.empty());
        check_true(job->workflow.outputs.empty());
        check_true(job->workflow.nodes[0].parameters.empty());
        check_true(job->workflow.nodes[1].parameters.empty());
        check_true(job->workflow.nodes[1].dependencies.empty());
        check_true(job->img_gen.gen_params.prompt.empty());
        const json terminal = make_async_job_json(manager, *job);
        check_equal(terminal["nodes"][0]["id"].get<std::string>(), std::string("first"));
        check_equal(terminal["nodes"][1]["id"].get<std::string>(), std::string("second"));
        check_equal(terminal["nodes"][0]["status"].get<std::string>(), std::string("skipped"));
        check_true(terminal["result"].is_null());
    }

    it("drops old Workflow V1 payloads after success and failure without changing snapshots")
    {
        for (bool success : {false, true}) {
            TestAsyncJobManager manager;
            std::shared_ptr<AsyncGenerationJob> job;
            {
                std::lock_guard<std::mutex> lock(manager.mutex);
                job = enqueue(manager, AsyncJobKind::Workflow);
                job->workflow.nodes[0].parameters = {
                    {"prompt", std::string(2048, 'x')},
                    {"width", 128}, {"height", 128}, {"steps", 2},
                };
                job->workflow.order = {0, 1};
                job->workflow.model_recipe = "private-recipe";
                check_true(start_next_async_job(manager) == job);
                job->workflow_node_states = {"completed", success ? "completed" : "running"};
                AsyncJobCompletion outcome = successful_output();
                outcome.success = success;
                outcome.error_message = success ? "" : "injected failure";
                check_true(finish_async_job(manager, *job, std::move(outcome)));
                check_true(job->workflow.nodes[0].parameters.empty());
                check_true(job->workflow.order.empty());
                check_true(job->workflow.model_recipe.empty());
            }
            const json snapshot = make_async_terminal_job_json(*job);
            check_equal(snapshot["nodes"].size(), static_cast<size_t>(2));
            check_equal(snapshot["nodes"][0]["id"].get<std::string>(), std::string("first"));
            if (success) {
                check_equal(snapshot["result"]["version"].get<int>(), 1);
                check_true(snapshot["error"].is_null());
            } else {
                check_true(snapshot["result"].is_null());
                check_equal(snapshot["nodes"][1]["status"].get<std::string>(),
                            std::string("failed"));
            }
        }
    }

    it("retires image and video decode inputs without allocating new default requests")
    {
        for (AsyncJobKind kind : {AsyncJobKind::ImgGen, AsyncJobKind::VidGen}) {
            TestAsyncJobManager manager;
            std::shared_ptr<AsyncGenerationJob> job;
            {
                std::lock_guard<std::mutex> lock(manager.mutex);
                job = enqueue(manager, kind);
                auto& request = kind == AsyncJobKind::ImgGen
                    ? job->img_gen.gen_params : job->vid_gen.gen_params;
                request.prompt = std::string(2048, 's');
                request.negative_prompt = std::string(2048, 'n');
                request.ref_image_paths = {"an-old-upload.png"};
                request.ref_audio_paths = {"an-old-audio.wav"};
                request.init_image.reset({1, 1, 3,
                    static_cast<uint8_t*>(std::malloc(3))});
                check_true(request.init_image.get().data != nullptr);
                request.lora_map["private-lora"] = 1.0f;
                request.extra_sample_args = "private-args";
                check_true(start_next_async_job(manager) == job);
                check_true(finish_async_job(manager, *job, successful_output(kind)));
                check_true(job->status == AsyncJobStatus::Completed);
                check_true(request.prompt.empty());
                check_true(request.negative_prompt.empty());
                check_true(request.ref_image_paths.empty());
                check_true(request.ref_audio_paths.empty());
                check_true(request.lora_map.empty());
                check_true(request.extra_sample_args.empty());
                check_true(request.skip_layers.empty());
                check_true(request.init_image.get().data == nullptr);
            }
            const json terminal = make_async_terminal_job_json(*job);
            check_equal(terminal["status"].get<std::string>(), std::string("completed"));
            check_true(terminal["error"].is_null());
            check_true(!terminal["result"].is_null());
        }
    }

    it("bounds native preview frame allocation without restricting generation")
    {
        check_true(sd_app_preview_frame_in_budget(1024, 1024, 3));
        check_true(sd_app_preview_frame_in_budget(2048, 2048, 4));
        check_true(sd_app_preview_frame_in_budget(4096, 1024, 1));
        check_false(sd_app_preview_frame_in_budget(2049, 2048, 3));
        check_false(sd_app_preview_frame_in_budget(8192, 8192, 3));
        check_false(sd_app_preview_frame_in_budget(0, 128, 3));
        check_false(sd_app_preview_frame_in_budget(-1, 128, 3));
        check_false(sd_app_preview_frame_in_budget(128, 0, 3));
        check_false(sd_app_preview_frame_in_budget(128, 128, 0));
        check_false(sd_app_preview_frame_in_budget(128, 128, 5));
        check_false(sd_app_preview_frame_in_budget(2147483647LL, 2147483647LL, 4));
    }

    it("scopes native task IDs to the exact model service instance")
    {
        TestAsyncJobManager old_process;
        TestAsyncJobManager current_process;
        current_process.instance_id = "fedcba9876543210fedcba9876543210";
        auto first = std::make_shared<AsyncGenerationJob>();
        auto second = std::make_shared<AsyncGenerationJob>();
        const auto old_receipt = admit_async_job(old_process, first);
        const auto current_receipt = admit_async_job(current_process, second);
        check_true(old_receipt.status == AsyncJobAdmissionStatus::Accepted);
        check_true(current_receipt.status == AsyncJobAdmissionStatus::Accepted);
        check_true(first->id != second->id);
        check_true(first->id.find(old_process.instance_id) != std::string::npos);
        check_true(second->id.find(current_process.instance_id) != std::string::npos);
        check_true(json::parse(old_receipt.receipt_json)["id"] == first->id);
        check_true(json::parse(current_receipt.receipt_json)["id"] == second->id);

        // A process-local registry must never reuse a prior process's job
        // identity even if the ordinal and timestamp happen to be the same.
        std::lock_guard<std::mutex> lock(current_process.mutex);
        check_true(current_process.jobs.find(first->id) == current_process.jobs.end());
        check_true(current_process.expired_jobs.find(first->id) ==
                   current_process.expired_jobs.end());
    }

    it("rejects admission before process identity initialization")
    {
        AsyncJobManager uninitialized;
        auto job = std::make_shared<AsyncGenerationJob>();
        bool invalid = false;
        try {
            (void)admit_async_job(uninitialized, job);
        } catch (const std::logic_error&) {
            invalid = true;
        }
        check_true(invalid);
        check_true(job->id.empty());
        check_true(uninitialized.jobs.empty());
        check_true(uninitialized.queue.empty());

        uninitialized.instance_id = "0000-not-lowercase-hex-0000000";
        invalid = false;
        try {
            (void)admit_async_job(uninitialized, job);
        } catch (const std::logic_error&) {
            invalid = true;
        }
        check_true(invalid);
        check_true(uninitialized.jobs.empty());
    }

    it("fails image completions with missing or mismatched metadata before terminal publication")
    {
        for (int malformed = 0; malformed < 3; ++malformed) {
            TestAsyncJobManager manager;
            std::lock_guard<std::mutex> lock(manager.mutex);
            auto job = enqueue(manager, AsyncJobKind::ImgGen);
            check_true(start_next_async_job(manager) == job);
            auto output = successful_output(AsyncJobKind::ImgGen);
            if (malformed == 0)
                output.result_generation_info = nullptr;
            else if (malformed == 1)
                output.result_generation_info = json::array();
            else
                output.result_images_b64 = {""};
            check_true(finish_async_job(manager, *job, std::move(output)));
            check_true(job->status == AsyncJobStatus::Failed);
            check_true(job->result_images_b64.empty());
            const json terminal = make_async_job_json(manager, *job);
            check_true(terminal["result"].is_null());
            check_equal(terminal["error"]["code"].get<std::string>(),
                        std::string("invalid_generation_result"));
        }
    }

    it("fails video completions missing media, fps, frames or metadata")
    {
        for (int malformed = 0; malformed < 5; ++malformed) {
            TestAsyncJobManager manager;
            std::lock_guard<std::mutex> lock(manager.mutex);
            auto job = enqueue(manager, AsyncJobKind::VidGen);
            check_true(start_next_async_job(manager) == job);
            auto output = successful_output(AsyncJobKind::VidGen);
            if (malformed == 0)
                output.result_media_b64.clear();
            else if (malformed == 1)
                output.result_media_mime_type.clear();
            else if (malformed == 2)
                output.result_fps = 0;
            else if (malformed == 3)
                output.result_frame_count = 0;
            else
                output.result_generation_info = nullptr;
            check_true(finish_async_job(manager, *job, std::move(output)));
            check_true(job->status == AsyncJobStatus::Failed);
            check_true(job->result_media_b64.empty());
            const json terminal = make_async_job_json(manager, *job);
            check_true(terminal["result"].is_null());
            check_equal(terminal["error"]["code"].get<std::string>(),
                        std::string("invalid_generation_result"));
        }
    }

    it("never publishes incomplete or unfinished Workflow V1 results as Completed")
    {
        for (int malformed = 0; malformed < 6; ++malformed) {
            TestAsyncJobManager manager;
            std::lock_guard<std::mutex> lock(manager.mutex);
            auto job = enqueue(manager, AsyncJobKind::Workflow);
            check_true(start_next_async_job(manager) == job);
            job->workflow_node_states = {"completed", "completed"};
            auto output = successful_output();
            if (malformed == 0)
                output.result_workflow = nullptr;
            else if (malformed == 1)
                output.result_workflow.erase("outputs");
            else if (malformed == 2)
                output.result_workflow["outputs"][0]["node_id"] = "other";
            else if (malformed == 3)
                output.result_workflow["outputs"][0]["images"] = json::array();
            else if (malformed == 4)
                output.result_workflow["outputs"][0]["images"][0]["b64_json"] = "";
            else
                job->workflow_node_states[1] = "running";
            check_true(finish_async_job(manager, *job, std::move(output)));
            check_true(job->status == AsyncJobStatus::Failed);
            check_equal(job->workflow_node_states[1], std::string(
                malformed == 5 ? "failed" : "completed"));
            const json terminal = make_async_terminal_job_json(*job);
            check_true(terminal["result"].is_null());
            check_equal(terminal["error"]["code"].get<std::string>(),
                        std::string("invalid_generation_result"));
        }
    }

    it("lets cancellation take priority over even an incomplete native result")
    {
        TestAsyncJobManager manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        auto job = enqueue(manager, AsyncJobKind::ImgGen);
        check_true(start_next_async_job(manager) == job);
        job->cancel_requested.store(true);
        AsyncJobCompletion output;
        output.success = true;
        check_true(finish_async_job(manager, *job, std::move(output)));
        check_true(job->status == AsyncJobStatus::Cancelled);
        const json terminal = make_async_terminal_job_json(*job);
        check_true(terminal["result"].is_null());
        check_equal(terminal["error"]["code"].get<std::string>(),
                    std::string("cancelled"));
    }

}
