#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <json.hpp>

// Workflow V1 is deliberately an ordering DAG. Media edges and additional
// providers require their own versioned contracts and are not implied by
// depends_on. No engine or runtime-state pointer is stored in this plan.
struct WorkflowV1Node {
    std::string id;
    std::vector<size_t> dependencies;
    nlohmann::json parameters;
};

struct WorkflowV1 {
    std::string model_recipe;
    std::string model_instance;
    uint64_t model_revision = 0;
    std::vector<WorkflowV1Node> nodes;
    std::vector<size_t> order;
    std::vector<size_t> outputs;
};

constexpr size_t k_workflow_v1_max_nodes = 4;
constexpr size_t k_workflow_v1_max_request_bytes = 64 * 1024;
constexpr size_t k_workflow_v1_max_result_bytes = 8 * 1024 * 1024;

// All schema and bounds checks run before runtime admission, including cycle,
// duplicate-ID, unknown-provider, unsupported-param, and integer-type rejection.
bool parse_workflow_v1(const nlohmann::json& document,
                       WorkflowV1& destination,
                       std::string& error);

enum class WorkflowV1Outcome { Completed, Failed, Cancelled };

// The injected callback is the *only* executor hook. Production calls the
// existing sd.cpp generation owner; tests use an in-memory deterministic hook.
// on_node is called before/after execution and may hold its own state lock.
// The caller owns the callback's data until the function returns.
WorkflowV1Outcome run_workflow_v1(
    const WorkflowV1& workflow,
    const std::function<bool(size_t, std::string&)>& execute,
    const std::function<bool()>& cancelled,
    const std::function<void(size_t, const char*)>& on_node,
    std::string& error);
