"""Exercise the real upstream HTTP server without weights or inference."""

import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def request_json(port, method, path, body=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        encoded = None if body is None else json.dumps(body).encode("utf-8")
        headers = {} if body is None else {"Content-Type": "application/json"}
        connection.request(method, path, body=encoded, headers=headers)
        response = connection.getresponse()
        payload = response.read(4 * 1024 * 1024 + 1)
        require(len(payload) <= 4 * 1024 * 1024, "HTTP smoke response exceeded 4 MiB")
        require(response.getheader("Content-Type", "").startswith("application/json"),
                path + " did not return JSON")
        document = json.loads(payload)
        require(isinstance(document, dict), path + " did not return a JSON object")
        return response.status, document
    finally:
        connection.close()


def exercise_server(executable):
    # Bind an ephemeral loopback port once; readiness polling does not restart
    # the process or retry model operations. An unavailable port fails the test.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="sd-http-smoke-") as temporary:
        model_directory = Path(temporary) / "models"
        model_directory.mkdir()
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(
                [str(executable), "--listen-ip", "127.0.0.1",
                 "--listen-port", str(port), "--model-dir", str(model_directory)],
                cwd=executable.parent, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while True:
                    require(process.poll() is None, "sd-server exited before becoming ready")
                    try:
                        code, status = request_json(port, "GET", "/sdcpp/v1/models/status")
                        break
                    except (OSError, http.client.HTTPException):
                        require(time.monotonic() < deadline, "sd-server readiness deadline expired")
                        time.sleep(0.05)

                require(code == 200 and status.get("state") == "empty",
                        "server must start with an empty model context")

                code, catalog = request_json(port, "GET", "/sdcpp/v1/recipes")
                require(code == 200 and isinstance(catalog.get("recipes"), list)
                        and len(catalog["recipes"]) > 0,
                        "built-in model recipes must be available without weights")

                code, files = request_json(port, "GET", "/sdcpp/v1/model-files")
                require(code == 200 and files.get("files") == [],
                        "the isolated model directory must have no weight files")

                code, capabilities = request_json(port, "GET", "/sdcpp/v1/capabilities")
                require(code == 200 and capabilities.get("runtime", {}).get("state") == "empty",
                        "capabilities must describe the empty runtime")
                features = capabilities.get("generation_features")
                require(isinstance(features, dict) and len(features) > 0
                        and all(value is False for value in features.values()),
                        "an empty runtime must not advertise loaded-model generation features")

                code, error = request_json(port, "POST", "/sdcpp/v1/img_gen", {})
                require(code == 503 and error.get("code") == "model_not_loaded",
                        "generation without a model must fail explicitly")

                code, error = request_json(port, "POST", "/sdcpp/v1/models/load", {})
                require(code == 400 and error.get("code") == "invalid_model_config",
                        "invalid model selection must be rejected")

                code, status = request_json(port, "POST", "/sdcpp/v1/models/unload", {})
                require(code == 200 and status.get("state") == "empty",
                        "unloading an empty context must preserve the empty state")
                require(process.poll() is None, "sd-server stopped while serving requests")
                print("Upstream HTTP catalog, empty-context capabilities and error paths passed.")
            except Exception:
                log.seek(0, 2)
                log.seek(max(0, log.tell() - 16384))
                sys.stderr.write(log.read().decode("utf-8", errors="replace"))
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)


if __name__ == "__main__":
    require(len(sys.argv) == 2, "usage: http_smoke.py /path/to/sd-server")
    executable = Path(sys.argv[1]).resolve(strict=True)
    exercise_server(executable)
