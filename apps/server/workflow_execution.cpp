#include "workflow_execution.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "async_jobs.h"
#include "model_catalog.h"
#include "workflow_v1.h"

namespace {
bool matches_model_binding(const WorkflowV1& workflow, const json& status) {
    if (status.value("state", std::string()) != "ready") return false;
    if (status.value("revision", uint64_t(0)) != workflow.model_revision ||
        status.value("instance", std::string()) != workflow.model_instance) return false;
    const json recipe = status.value("recipe", json(nullptr));
    if (recipe.is_null())
        return workflow.model_recipe == "@command-line" &&
               status.value("origin", std::string()) == "command_line";
    return recipe.is_string() && recipe.get<std::string>() == workflow.model_recipe;
}

bool make_node_request(const WorkflowV1Node& node,
                       ServerRuntime& runtime,
                       ImgGenJobRequest& request,
                       std::string& error) {
    // Preserve existing recipe and engine sampling defaults. V1 is deliberately
    // text-to-image, one PNG per node: defaults needing extra input/cycles must
    // be rejected, not silently dropped or interpreted as new media edges.
    request.gen_params = *runtime.default_gen_params;
    auto& params = request.gen_params;
    if (params.batch_count != 1 || params.hires_enabled ||
        params.init_image.get().data != nullptr || !params.init_image_path.empty() ||
        !params.ref_images.empty() || !params.ref_image_paths.empty() ||
        params.mask_image.get().data != nullptr || !params.mask_image_path.empty() ||
        params.control_image.get().data != nullptr || !params.control_image_path.empty() ||
        params.ip_adapter_image.get().data != nullptr || !params.ip_adapter_image_path.empty()) {
        error = "workflow V1 requires text-to-image defaults (batch_count=1, no image inputs or hires)";
        return false;
    }
    const auto& input = node.parameters;
    params.prompt = input.at("prompt").get<std::string>();
    if (input.contains("negative_prompt"))
        params.negative_prompt = input.at("negative_prompt").get<std::string>();
    params.width  = input.at("width").get<int>();
    params.height = input.at("height").get<int>();
    params.sample_params.sample_steps = input.at("steps").get<int>();
    if (input.contains("seed"))
        params.seed = input.at("seed").get<int64_t>();
    if (input.contains("cfg_scale"))
        params.sample_params.guidance.txt_cfg = input.at("cfg_scale").get<float>();

    request.output_format = "png";
    request.output_compression = 100;
    request.preview_mode = "none";
    request.adetailer_enabled = false;  // V1 never installs a second image-processing pass.
    if (!params.resolve_and_validate(IMG_GEN, "", runtime.ctx_params->hires_upscalers_dir, true) ||
        !validate_server_img_gen_request(runtime, request, error)) {
        if (error.empty()) error = "invalid sd.cpp generation parameters for workflow";
        return false;
    }
    return true;
}
} // namespace

bool validate_workflow_v1_admission(ServerRuntime& runtime,
                                    const WorkflowV1& workflow,
                                    std::string& error) {
    error.clear();
    for (const auto& node : workflow.nodes) {
        ImgGenJobRequest request;
        if (!make_node_request(node, runtime, request, error)) return false;
    }
    return true;
}

bool execute_workflow_job(ServerRuntime& runtime,
                          AsyncGenerationJob& job,
                          json& result,
                          std::string& error,
                          size_t max_output_base64_bytes) {
    const WorkflowV1& workflow = job.workflow;
    if (!runtime.sd_ctx || !matches_model_binding(workflow, model_status(runtime))) {
        error = "model binding changed before execution";
        return false;
    }

    json selected = json::object();
    size_t resident_bytes = 0;
    size_t retained_b64_bytes = 0;
    const auto execute = [&](size_t index, std::string& reason) -> bool {
        const auto& node = workflow.nodes[index];
        ImgGenJobRequest request;
        if (!make_node_request(node, runtime, request, reason)) return false;

        // The *same* owning job and cancellation flag are passed to the existing
        // sd.cpp generation monitor, so running cancellation reaches the native
        // context. No independent worker, callback, or inference provider.
        job.img_gen = std::move(request);
        const bool selected_output =
            std::find(workflow.outputs.begin(), workflow.outputs.end(), index) !=
            workflow.outputs.end();
        const size_t remaining_media = retained_b64_bytes <= max_output_base64_bytes
            ? max_output_base64_bytes - retained_b64_bytes : 0;
        const size_t remaining_workflow = resident_bytes <= k_workflow_v1_max_result_bytes
            ? k_workflow_v1_max_result_bytes - resident_bytes : 0;

        std::vector<std::string> images;
        json metadata;
        // depends_on is ordering only: a non-output node must not spend RAM
        // encoding/base64-ing pixels that cannot feed a later node.
        if (!execute_img_gen_job(runtime, job, images, metadata, reason,
                                 std::min(remaining_media, remaining_workflow),
                                 selected_output))
            return false;
        if (!selected_output)
            return true;
        if (images.size() != 1 || !metadata.is_array() || metadata.size() != 1) {
            reason = "workflow V1 generation returned an unexpected image count";
            return false;
        }
        const size_t metadata_bytes = metadata.dump().size();
        if (metadata_bytes > remaining_workflow ||
            images.front().size() > remaining_workflow - metadata_bytes) {
            reason = "workflow V1 result exceeds 8 MiB retained output budget";
            return false;
        }
        resident_bytes += metadata_bytes + images.front().size();
        retained_b64_bytes += images.front().size();
        json image = {{"index", 0},
                      {"b64_json", std::move(images.front())},
                      {"generation_info", std::move(metadata[0])}};
        selected[node.id] = json::array({std::move(image)});
        return true;
    };
    const auto cancelled = [&]() { return job.cancel_requested.load(); };
    const auto on_node = [&](size_t index, const char* state) {
        std::lock_guard<std::mutex> lock(runtime.async_job_manager->mutex);
        job.workflow_node_states[index] = state;
    };

    const auto outcome = run_workflow_v1(workflow, execute, cancelled, on_node, error);
    if (outcome != WorkflowV1Outcome::Completed) return false;

    json output_items = json::array();
    for (const size_t index : workflow.outputs) {
        const std::string& id = workflow.nodes[index].id;
        output_items.push_back({{"node_id", id}, {"images", std::move(selected.at(id))}});
    }
    result = {{"version", 1},
              {"model", {{"recipe", workflow.model_recipe}, {"revision", workflow.model_revision}, {"instance", workflow.model_instance}}},
              {"outputs", std::move(output_items)}};
    return true;
}
