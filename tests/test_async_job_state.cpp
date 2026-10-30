#include "async_jobs.h"
#include "tinytest.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    }
    manager.jobs.emplace(job->id, job);
    manager.queue.push_back(job->id);
    return job;
}

static AsyncJobCompletion successful_output() {
    AsyncJobCompletion output;
    output.success = true;
    output.result_images_b64 = {"encoded"};
    output.result_generation_info = json::array({json::object()});
    output.result_workflow = {{"version", 1}};
    return output;
}

spec("AsyncJobManager native terminal ownership and fault injection")
{
    it("publishes a failed native result without leaking partial media or completing dependents")
    {
        AsyncJobManager manager;
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
        AsyncJobManager manager;
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
        AsyncJobManager manager;
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
        AsyncJobManager manager;
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
        AsyncJobManager manager;
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
            AsyncJobManager manager;
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
        AsyncJobManager manager;
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
}
