#pragma once

#include <string>

#include "runtime.h"
#include "workflow_v1.h"

struct AsyncGenerationJob;

// Validate all nodes against the currently loaded recipe defaults before
// publishing a job to the queue. Does not create a context or generate pixels.
bool validate_workflow_v1_admission(ServerRuntime& runtime,
                                    const WorkflowV1& workflow,
                                    std::string& error);

// Runs the already-admitted, model-bound workflow on the existing single
// AsyncJobManager worker. It uses execute_img_gen_job, never a second engine.
bool execute_workflow_job(ServerRuntime& runtime,
                          AsyncGenerationJob& job,
                          json& result,
                          std::string& error);
