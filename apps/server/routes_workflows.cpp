#include "routes.h"

#include <memory>
#include <string>

#include "async_jobs.h"
#include "model_catalog.h"
#include "workflow_v1.h"
#include "workflow_execution.h"
#include "workflow_http_guard.h"

namespace {
void workflow_error(httplib::Response& response, int status,
                    const char* code, const std::string& message) {
    response.status = status;
    response.set_content(json({{"code", code}, {"error", message}}).dump(), "application/json");
}

bool model_matches(const ServerRuntime& runtime, const WorkflowV1& workflow, const json& status) {
    if (!runtime.sd_ctx || status.value("state", std::string()) != "ready") return false;
    if (status.value("revision", uint64_t(0)) != workflow.model_revision ||
        status.value("instance", std::string()) != workflow.model_instance) return false;
    const json recipe = status.value("recipe", json(nullptr));
    if (recipe.is_null())
        return workflow.model_recipe == "@command-line" &&
               status.value("origin", std::string()) == "command_line";
    return recipe.is_string() && recipe.get<std::string>() == workflow.model_recipe;
}
} // namespace

void register_workflow_endpoints(httplib::Server& server, ServerRuntime& runtime) {
    // This provisional /sdcpp endpoint is strictly single-process/local.
    // It is NOT the proposed authenticated CHttp /v1 workflow service.
    RuntimeRoutes routes(server, runtime);
    routes.post("/sdcpp/v1/workflows", [&runtime](const httplib::Request& req, httplib::Response& res) {
        // The inherited httplib baseline has no identity/tenant authorization.
        // Never advertise an unauthenticated workflow route on a wide-area bind.
        if (!workflow_local_request_allowed(req, *runtime.svr_params)) {
            workflow_error(res, 403, "workflow_local_only",
                           "Workflow V1 requires a direct native loopback request without Origin/Referer");
            return;
        }
        if (req.body.size() > k_workflow_v1_max_request_bytes) {
            workflow_error(res, 413, "workflow_too_large", "workflow exceeds 64 KiB JSON limit");
            return;
        }
        WorkflowV1 workflow;
        std::string error;
        try {
            json payload = json::parse(req.body);
            if (!parse_workflow_v1(payload, workflow, error)) {
                workflow_error(res, 400, "invalid_workflow", error);
                return;
            }
            if (!model_matches(runtime, workflow, model_status(runtime))) {
                workflow_error(res, runtime.sd_ctx ? 409 : 503,
                               runtime.sd_ctx ? "model_binding_mismatch" : "model_not_loaded",
                               "workflow requires the exact loaded model recipe and revision");
                return;
            }
            if (!runtime_supports_generation_mode(runtime, IMG_GEN)) {
                workflow_error(res, 422, "unsupported_generation_mode",
                               unsupported_generation_mode_error(IMG_GEN));
                return;
            }

            if (!validate_workflow_v1_admission(runtime, workflow, error)) {
                workflow_error(res, 422, "unsupported_workflow_defaults", error);
                return;
            }

            auto& manager = *runtime.async_job_manager;
            auto job = std::make_shared<AsyncGenerationJob>();
            job->kind = AsyncJobKind::Workflow;
            job->workflow = std::move(workflow);
            job->workflow_node_states.assign(job->workflow.nodes.size(), "queued");
            // RuntimeRoutes::protect holds the shared model-transition lock
            // through this single manager admission commit.
            const AsyncJobAdmission receipt = admit_async_job(manager, job);
            if (receipt.status != AsyncJobAdmissionStatus::Accepted) {
                const bool stopping = receipt.status == AsyncJobAdmissionStatus::Stopping;
                workflow_error(res, stopping ? 503 : 429,
                               stopping ? "shutting_down" : "workflow_capacity",
                               stopping ? "worker admission closed" : "workflow or job queue is full");
                return;
            }
            res.status = 202;
            res.set_content(receipt.receipt_json, "application/json");
        } catch (const json::parse_error& e) {
            workflow_error(res, 400, "invalid_json", e.what());
        } catch (const std::exception& e) {
            workflow_error(res, 500, "workflow_admission_failed", e.what());
        }
    });
}
