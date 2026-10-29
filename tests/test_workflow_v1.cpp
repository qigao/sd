#include "workflow_v1.h"
#include "tinytest.hpp"

#include <atomic>
#include <string>
#include <stdexcept>
#include <vector>

using json = nlohmann::json;

static json sample() {
    return {
        {"version", 1},
        {"model", {{"recipe", "@command-line"}, {"revision", 0}, {"instance", "0123456789abcdef0123456789abcdef"}}},
        {"nodes", json::array({
            {{"id", "second"}, {"type", "sdcpp.image.generate"},
             {"depends_on", json::array({"first"})},
             {"params", {{"prompt", "another mountain"}, {"width", 128}, {"height", 128}, {"steps", 2}}}},
            {{"id", "first"}, {"type", "sdcpp.image.generate"},
             {"depends_on", json::array()},
             {"params", {{"prompt", "a mountain"}, {"width", 128}, {"height", 128}, {"steps", 2}}}}
        })},
        {"outputs", json::array({"second"})}
    };
}

spec("JSON Workflow V1 native validation and scheduler")
{
    it("parses an out-of-order dependency graph and executes in topological order")
    {
        WorkflowV1 graph;
        std::string error;
        check_true(parse_workflow_v1(sample(), graph, error));
        check_equal(graph.nodes.size(), static_cast<size_t>(2));
        check_equal(graph.order.size(), static_cast<size_t>(2));
        check_equal(graph.nodes[graph.order[0]].id, std::string("first"));
        std::vector<std::string> visited;
        const auto outcome = run_workflow_v1(
            graph,
            [&](size_t index, std::string&) {
                visited.push_back(graph.nodes[index].id);
                return true;
            },
            [] { return false; },
            [](size_t, const char*) {},
            error);
        check_true(outcome == WorkflowV1Outcome::Completed);
        check_equal(visited.size(), static_cast<size_t>(2));
        check_equal(visited[1], std::string("second"));
    }

    it("rejects unsupported providers, unknown fields and malformed values")
    {
        WorkflowV1 graph;
        std::string error;
        auto doc = sample();
        doc["nodes"][0]["type"] = "kfcore.image";
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["params"]["arbitrary_path"] = "/tmp/input";
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["params"]["steps"] = 1.5;
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["params"]["width"] = 4096;
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["params"]["cfg_scale"] = "7";
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["model"].erase("instance");
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["model"]["instance"] = "NON_HEX";
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["model"]["instance"] = json::object();
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["version"] = 2;
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["extra"] = true;
        check_false(parse_workflow_v1(doc, graph, error));
    }

    it("rejects cycles, unknown and duplicate dependencies and outputs")
    {
        WorkflowV1 graph;
        std::string error;
        auto doc = sample();
        doc["nodes"][1]["depends_on"] = json::array({"second"});
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["depends_on"] = json::array({"absent"});
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["depends_on"] = json::array({"first", "first"});
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["outputs"] = json::array({"second", "second"});
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["id"] = "first";
        check_false(parse_workflow_v1(doc, graph, error));
    }

    it("bounds output, prompt and graph size")
    {
        WorkflowV1 graph;
        std::string error;
        auto doc = sample();
        doc["nodes"][0]["params"]["prompt"] = std::string(2049, 'x');
        check_false(parse_workflow_v1(doc, graph, error));
        doc = sample();
        doc["nodes"][0]["params"]["width"] = 1024;
        doc["nodes"][0]["params"]["height"] = 1024;
        doc["nodes"][1]["params"]["width"] = 1024;
        doc["nodes"][1]["params"]["height"] = 1024;
        check_true(parse_workflow_v1(doc, graph, error));
        doc["nodes"].push_back(doc["nodes"][1]);
        doc["nodes"][2]["id"] = "third";
        check_false(parse_workflow_v1(doc, graph, error));
    }

    it("stops after failure and marks successors skipped")
    {
        WorkflowV1 graph;
        std::string error;
        check_true(parse_workflow_v1(sample(), graph, error));
        int invoked = 0;
        std::vector<std::string> changes;
        auto outcome = run_workflow_v1(
            graph,
            [&](size_t, std::string& reason) {
                ++invoked;
                reason = "injected failure";
                return false;
            },
            [] { return false; },
            [&](size_t, const char* status) { changes.emplace_back(status); },
            error);
        check_true(outcome == WorkflowV1Outcome::Failed);
        check_equal(invoked, 1);
        check_equal(error, std::string("injected failure"));
        check_equal(changes.back(), std::string("skipped"));
    }

    it("honors cancellation between nodes and never executes the successor")
    {
        WorkflowV1 graph;
        std::string error;
        check_true(parse_workflow_v1(sample(), graph, error));
        std::atomic<bool> cancelled{false};
        int invoked = 0;
        auto outcome = run_workflow_v1(
            graph,
            [&](size_t, std::string&) {
                ++invoked;
                cancelled.store(true);
                return true;
            },
            [&] { return cancelled.load(); },
            [](size_t, const char*) {},
            error);
        check_true(outcome == WorkflowV1Outcome::Cancelled);
        check_equal(invoked, 1);
    }

    it("converts node exceptions into a terminal failure")
    {
        WorkflowV1 graph;
        std::string error;
        check_true(parse_workflow_v1(sample(), graph, error));
        const auto outcome = run_workflow_v1(
            graph,
            [](size_t, std::string&) -> bool { throw std::runtime_error("injected exception"); },
            [] { return false; },
            [](size_t, const char*) {},
            error);
        check_true(outcome == WorkflowV1Outcome::Failed);
        check_equal(error, std::string("injected exception"));
    }
}
