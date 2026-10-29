#pragma once

#include <string>

#include "runtime.h"

struct AsyncGenerationJob;

// Runs the already-admitted, model-bound workflow on the existing single
// AsyncJobManager worker. It uses execute_img_gen_job, never a second engine.
bool execute_workflow_job(ServerRuntime& runtime,
                          AsyncGenerationJob& job,
                          json& result,
                          std::string& error);
