#include "workflow_v1.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

void keys(const json& object, std::initializer_list<const char*> accepted, const std::string& where) {
    require(object.is_object(), where + " must be an object");
    std::set<std::string> allowed(accepted.begin(), accepted.end());
    for (auto it = object.begin(); it != object.end(); ++it) {
        require(allowed.count(it.key()) != 0, where + ": unknown field '" + it.key() + "'");
    }
}

std::string required_text(const json& obj, const char* field, size_t max, const std::string& where) {
    require(obj.contains(field) && obj.at(field).is_string(), where + "." + field + " must be a string");
    std::string value = obj.at(field).get<std::string>();
    require(!value.empty() && value.size() <= max, where + "." + field + " has invalid length");
    return value;
}

bool valid_id(const std::string& name) {
    if (name.empty() || name.size() > 48 ||
        !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

int64_t integer(const json& value, int64_t minimum, int64_t maximum, const std::string& where) {
    require(value.is_number_integer(), where + " must be an integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        require(number <= static_cast<uint64_t>(maximum) &&
                    (minimum <= 0 || number >= static_cast<uint64_t>(minimum)),
                where + " out of range");
        return static_cast<int64_t>(number);
    }
    const auto number = value.get<int64_t>();
    require(number >= minimum && number <= maximum, where + " out of range");
    return number;
}

void validate_parameters(const json& params) {
    keys(params, {"prompt", "negative_prompt", "width", "height", "seed", "steps", "cfg_scale"}, "params");
    required_text(params, "prompt", 2048, "params");
    if (params.contains("negative_prompt")) {
        require(params.at("negative_prompt").is_string() &&
                    params.at("negative_prompt").get<std::string>().size() <= 2048,
                "params.negative_prompt must be a string of at most 2048 bytes");
    }
    for (const char* field : {"width", "height"}) {
        require(params.contains(field), std::string("params.") + field + " is required");
        const auto value = integer(params.at(field), 64, 1024, std::string("params.") + field);
        require(value % 8 == 0, std::string("params.") + field + " must be divisible by 8");
    }
    require(params.contains("steps"), "params.steps is required");
    integer(params.at("steps"), 1, 100, "params.steps");
    if (params.contains("seed")) {
        integer(params.at("seed"), -1, std::numeric_limits<int64_t>::max(), "params.seed");
    }
    if (params.contains("cfg_scale")) {
        const auto& value = params.at("cfg_scale");
        require(value.is_number(), "params.cfg_scale must be a number");
        const double scale = value.get<double>();
        require(std::isfinite(scale) && scale >= 0.0 && scale <= 30.0,
                "params.cfg_scale must be finite and between 0 and 30");
    }
}
} // namespace

bool parse_workflow_v1(const nlohmann::json& document,
                       WorkflowV1& destination,
                       std::string& error) {
    destination = {};
    error.clear();
    try {
        keys(document, {"version", "model", "nodes", "outputs"}, "workflow");
        require(document.contains("version") &&
                    integer(document.at("version"), 1, 1, "version") == 1,
                "workflow version 1 is required");
        const json& model = document.at("model");
        keys(model, {"recipe", "revision"}, "model");
        WorkflowV1 parsed;
        parsed.model_recipe = required_text(model, "recipe", 128, "model");
        require(model.contains("revision"), "model.revision is required");
        parsed.model_revision = static_cast<uint64_t>(
            integer(model.at("revision"), 0, std::numeric_limits<int64_t>::max(), "model.revision"));

        const json& nodes = document.at("nodes");
        require(nodes.is_array() && !nodes.empty() && nodes.size() <= k_workflow_v1_max_nodes,
                "nodes must contain 1-4 entries");
        std::map<std::string, size_t> indices;
        std::vector<std::vector<std::string>> unresolved;
        uint64_t total_pixels = 0;
        for (const auto& value : nodes) {
            keys(value, {"id", "type", "depends_on", "params"}, "node");
            WorkflowV1Node node;
            node.id = required_text(value, "id", 48, "node");
            require(valid_id(node.id), "node.id must be an ASCII identifier");
            require(indices.emplace(node.id, parsed.nodes.size()).second, "duplicate node id: " + node.id);
            require(required_text(value, "type", 80, "node") == "sdcpp.image.generate",
                    "unsupported node type (only sdcpp.image.generate is implemented)");
            require(value.contains("params"), "node.params is required");
            validate_parameters(value.at("params"));
            node.parameters = value.at("params");
            total_pixels += static_cast<uint64_t>(node.parameters.at("width").get<int>()) *
                            static_cast<uint64_t>(node.parameters.at("height").get<int>());
            require(total_pixels <= 2ULL * 1024 * 1024,
                    "workflow exceeds 2 megapixels of admitted node output");
            require(value.contains("depends_on") && value.at("depends_on").is_array(),
                    "node.depends_on must be an array");
            const auto& dependencies = value.at("depends_on");
            require(dependencies.size() < k_workflow_v1_max_nodes,
                    "node.depends_on exceeds the node limit");
            std::vector<std::string> names;
            for (const auto& entry : dependencies) {
                require(entry.is_string(), "dependency must be a node id string");
                const auto name = entry.get<std::string>();
                require(valid_id(name), "invalid dependency id");
                require(std::find(names.begin(), names.end(), name) == names.end(),
                        "duplicate dependency: " + name);
                names.push_back(name);
            }
            unresolved.push_back(std::move(names));
            parsed.nodes.push_back(std::move(node));
        }

        for (size_t index = 0; index < parsed.nodes.size(); ++index) {
            for (const auto& name : unresolved[index]) {
                const auto it = indices.find(name);
                require(it != indices.end(), "unknown dependency: " + name);
                require(it->second != index, "node depends on itself: " + name);
                parsed.nodes[index].dependencies.push_back(it->second);
            }
        }
        std::vector<bool> scheduled(parsed.nodes.size(), false);
        while (parsed.order.size() != parsed.nodes.size()) {
            bool progressed = false;
            for (size_t i = 0; i < parsed.nodes.size(); ++i) {
                if (scheduled[i]) continue;
                bool ready = true;
                for (const size_t dep : parsed.nodes[i].dependencies) {
                    if (!scheduled[dep]) {
                        ready = false;
                        break;
                    }
                }
                if (!ready) continue;
                scheduled[i] = true;
                parsed.order.push_back(i);
                progressed = true;
                break;
            }
            require(progressed, "workflow dependency graph contains a cycle");
        }

        const json& outputs = document.at("outputs");
        require(outputs.is_array() && !outputs.empty() &&
                    outputs.size() <= parsed.nodes.size(),
                "outputs must list 1-4 distinct node ids");
        std::set<size_t> unique_outputs;
        for (const auto& value : outputs) {
            require(value.is_string(), "output must be a node id string");
            const auto name = value.get<std::string>();
            const auto it = indices.find(name);
            require(it != indices.end(), "unknown output: " + name);
            require(unique_outputs.insert(it->second).second, "duplicate output: " + name);
            parsed.outputs.push_back(it->second);
        }
        destination = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        destination = {};
        error = e.what();
        return false;
    }
}

WorkflowV1Outcome run_workflow_v1(
    const WorkflowV1& workflow,
    const std::function<bool(size_t, std::string&)>& execute,
    const std::function<bool()>& cancelled,
    const std::function<void(size_t, const char*)>& on_node,
    std::string& error) {
    error.clear();
    for (size_t i = 0; i < workflow.order.size(); ++i) {
        const size_t index = workflow.order[i];
        if (cancelled()) {
            for (size_t rest = i; rest < workflow.order.size(); ++rest)
                on_node(workflow.order[rest], "skipped");
            error = "workflow cancelled by client";
            return WorkflowV1Outcome::Cancelled;
        }
        on_node(index, "running");
        bool success = false;
        try {
            success = execute(index, error);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown workflow node exception";
        }
        if (!success || cancelled()) {
            const bool was_cancelled = cancelled();
            on_node(index, was_cancelled ? "cancelled" : "failed");
            for (size_t rest = i + 1; rest < workflow.order.size(); ++rest)
                on_node(workflow.order[rest], "skipped");
            if (error.empty()) {
                error = was_cancelled ? "workflow cancelled by client" : "workflow node failed";
            }
            return was_cancelled ? WorkflowV1Outcome::Cancelled : WorkflowV1Outcome::Failed;
        }
        on_node(index, "completed");
    }
    return WorkflowV1Outcome::Completed;
}
