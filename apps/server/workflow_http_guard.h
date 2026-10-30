#pragma once

#include <string>

#include "httplib.h"
#include "runtime.h"

// A temporary local-only gate for the inherited httplib service. This is not
// authentication. The future CHttp Service must authorize per caller/job.
// Reject browser Origin/Referer and unexpected Host to avoid reusing the
// baseline's permissive credentialed CORS for a new workflow write/result API.
inline bool workflow_local_request_allowed(const httplib::Request& request,
                                           const SDSvrParams& config) {
    if (config.listen_ip != "127.0.0.1" && config.listen_ip != "::1")
        return false;
    if (request.has_header("Origin") || request.has_header("Referer"))
        return false;

    const std::string host = request.get_header_value("Host");
    const std::string port = std::to_string(config.listen_port);
    return host == "127.0.0.1:" + port || host == "localhost:" + port ||
           host == "[::1]:" + port;
}
