"""Opt-in real-weight qualification for the owned native Workflow V1 HTTP service.

No test model is downloaded or synthesized. A caller-supplied manifest pins
every selected component's SHA-256. The test fails (never silently skips) when
configured weights cannot be loaded, generated or cancelled as required.

Python is a test client only; native inference stays inside sd-app-server.
"""
import base64
import binascii
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


MAX_HTTP_BODY = 16 * 1024 * 1024
SHA256 = re.compile(r"^[0-9a-f]{64}$")
TERMINAL = {"completed", "failed", "cancelled"}


def require(condition, explanation):
    if not condition:
        raise RuntimeError(explanation)


def exactly(obj, fields, name):
    require(isinstance(obj, dict), name + " must be an object")
    extra = set(obj) - set(fields)
    require(not extra, name + " contains unknown keys: " + repr(sorted(extra)))


def in_root(root, relative):
    require(isinstance(relative, str) and relative and not Path(relative).is_absolute(),
            "manifest component path must be relative to model_root")
    path = (root / relative).resolve(strict=True)
    require(os.path.commonpath([str(root), str(path)]) == str(root) and path.is_file(),
            "component escapes model root or is not a regular file: " + relative)
    return path


def read_manifest(path):
    with path.open("rb") as stream:
        raw = stream.read(64 * 1024 + 1)
    require(len(raw) <= 64 * 1024, "qualification manifest exceeds 64 KiB")
    doc = json.loads(raw)
    exactly(doc, ("version", "model_root", "sha256", "load", "generation",
                  "cancel_steps"), "manifest")
    require(type(doc.get("version")) is int and doc["version"] == 1, "manifest.version must be 1")
    require(isinstance(doc.get("model_root"), str) and
            Path(doc["model_root"]).is_absolute(), "manifest.model_root must be absolute")
    root = Path(doc["model_root"]).resolve(strict=True)
    require(root.is_dir(), "manifest.model_root must be an existing directory")
    files = doc.get("sha256")
    require(isinstance(files, dict) and 1 <= len(files) <= 24,
            "manifest.sha256 requires 1-24 component digests")
    for relative, expected in files.items():
        require(isinstance(expected, str) and SHA256.fullmatch(expected),
                "manifest sha256 values must be lowercase 64-character hex")
        component = in_root(root, relative)
        h = hashlib.sha256()
        with component.open("rb") as stream:
            while True:
                chunk = stream.read(4 * 1024 * 1024)
                if not chunk:
                    break
                h.update(chunk)
        require(h.hexdigest() == expected, "weight digest mismatch: " + relative)
        print("Verified SHA-256:", relative, expected, flush=True)
    load = doc.get("load")
    exactly(load, ("recipe", "models"), "manifest.load")
    require(isinstance(load.get("recipe"), str) and load["recipe"],
            "manifest.load.recipe must be nonempty")
    require(isinstance(load.get("models"), dict) and load["models"] and
            all(isinstance(k, str) and isinstance(v, str) and v
                for k, v in load["models"].items()),
            "manifest.load.models must map model slot names to catalog file IDs")
    generation = doc.get("generation")
    exactly(generation, ("prompt", "width", "height", "steps", "seed", "cfg_scale"),
            "manifest.generation")
    require(isinstance(generation.get("prompt"), str) and generation["prompt"],
            "manifest.generation.prompt is required")
    for name in ("width", "height", "steps"):
        require(type(generation.get(name)) is int, "generation." + name + " must be an integer")
    require(64 <= generation["width"] <= 1024 and generation["width"] % 8 == 0 and
            64 <= generation["height"] <= 1024 and generation["height"] % 8 == 0 and
            1 <= generation["steps"] <= 100, "generation dimensions/steps out of V1 bounds")
    if "seed" in generation:
        require(type(generation["seed"]) is int, "generation.seed must be integer")
    if "cfg_scale" in generation:
        require(type(generation["cfg_scale"]) in (int, float) and
                0 <= generation["cfg_scale"] <= 30,
                "generation.cfg_scale out of bounds")
    cancel_steps = doc.get("cancel_steps", 48)
    require(type(cancel_steps) is int and generation["steps"] <= cancel_steps <= 100,
            "cancel_steps must be between generation.steps and 100")
    return root, files, load, generation, cancel_steps


def api(port, method, url, body=None, timeout=10, extra_headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        encoded = json.dumps(body, allow_nan=False).encode("utf-8") if body is not None else None
        headers = {"Content-Type": "application/json"} if encoded is not None else {}
        headers.update(extra_headers or {})
        connection.request(method, url, body=encoded, headers=headers)
        response = connection.getresponse()
        payload = response.read(MAX_HTTP_BODY + 1)
        require(len(payload) <= MAX_HTTP_BODY, "HTTP response exceeded 16 MiB: " + url)
        require(response.getheader("Content-Type", "").startswith("application/json"),
                "expected application/json for " + url)
        document = json.loads(payload)
        require(isinstance(document, dict), "server response is not an object: " + url)
        return response.status, document
    finally:
        connection.close()


def wait_ready(port, process, deadline):
    while time.monotonic() < deadline:
        require(process.poll() is None, "native server exited during startup")
        try:
            return api(port, "GET", "/sdcpp/v1/models/status", timeout=2)
        except (OSError, http.client.HTTPException):
            time.sleep(0.1)
    raise RuntimeError("native server readiness timeout")


def wait_job(port, job_id, deadline):
    while time.monotonic() < deadline:
        code, state = api(port, "GET", "/sdcpp/v1/jobs/" + job_id)
        require(code == 200 and state.get("id") == job_id, "job polling lost the admission")
        if state.get("status") in TERMINAL:
            return state
        time.sleep(0.1)
    raise RuntimeError("job failed to reach terminal state before qualification deadline: " + job_id)


def make_plan(status, generation, two=True):
    model = {"recipe": status["recipe"], "revision": status["revision"],
             "instance": status["instance"]}
    first = {"id": "source", "type": "sdcpp.image.generate",
             "depends_on": [], "params": dict(generation)}
    if not two:
        return {"version": 1, "model": model, "nodes": [first], "outputs": ["source"]}
    second = {"id": "final", "type": "sdcpp.image.generate",
              "depends_on": ["source"], "params": dict(generation)}
    # Deliberately out of source order: parser must topologically sort.
    return {"version": 1, "model": model,
            "nodes": [second, first], "outputs": ["final"]}


MAX_PNG_BYTES = 8 * 1024 * 1024


def _png_paeth(left, above, upper_left):
    # PNG filter 4: ties prefer left, then above, then upper-left.
    predicted = left + above - upper_left
    dl, du, dul = abs(predicted - left), abs(predicted - above), abs(predicted - upper_left)
    if dl <= du and dl <= dul:
        return left
    if du <= dul:
        return above
    return upper_left


def check_png(encoded, width, height):
    """Decode actual PNG IDAT pixels with bounded memory, not just chunk CRCs.

    This is a small test-only 8-bit, non-interlaced decoder for the native
    encoder's grayscale/GA/RGB/RGBA output. Unexpected layouts fail closed.
    No PIL dependency and no alternate image-generation provider.
    """
    require(isinstance(encoded, (str, bytes)) and
            len(encoded) <= ((MAX_PNG_BYTES + 2) // 3) * 4 + 4,
            "generated PNG base64 exceeds media budget")
    try:
        payload = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise RuntimeError("invalid base64 generated image") from exc
    require(8 < len(payload) <= MAX_PNG_BYTES, "PNG payload exceeds media budget")
    require(payload.startswith(b"\x89PNG\r\n\x1a\n"), "generated media is not PNG")

    offset, dimensions, channels = 8, None, None
    idat_chunks, idat_length = [], 0
    saw_idat, ended_idat, saw_end = False, False, False
    while offset + 12 <= len(payload):
        length = struct.unpack_from(">I", payload, offset)[0]
        require(length <= MAX_PNG_BYTES, "PNG chunk exceeds media budget")
        end = offset + 12 + length
        require(end <= len(payload), "truncated PNG chunk")
        kind = payload[offset + 4:offset + 8]
        data = payload[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", payload, offset + 8 + length)[0]
        require(zlib.crc32(kind + data) & 0xffffffff == crc, "PNG CRC mismatch")
        if offset == 8:
            require(kind == b"IHDR" and length == 13, "PNG IHDR missing")
            w, h, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", data)
            require(w > 0 and h > 0 and (w, h) == (width, height),
                    "actual PNG dimensions differ from request")
            require(depth == 8 and color in (0, 2, 4, 6) and
                    compression == 0 and filtering == 0 and interlace == 0,
                    "unsupported PNG pixel layout (8-bit non-interlaced required)")
            dimensions = (w, h)
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
        elif kind == b"IHDR":
            raise RuntimeError("duplicate PNG IHDR")
        if kind == b"IDAT":
            require(not ended_idat, "non-consecutive PNG IDAT chunks")
            idat_length += length
            require(idat_length <= MAX_PNG_BYTES, "PNG IDAT exceeds media budget")
            idat_chunks.append(data)
            saw_idat = True
        elif saw_idat and kind != b"IEND":
            ended_idat = True
        if kind == b"IEND":
            require(length == 0, "PNG IEND must be empty")
            saw_end = True
            offset = end
            break
        offset = end

    require(saw_idat and saw_end and offset == len(payload),
            "PNG missing IDAT/IEND or trailing bytes")
    require(dimensions is not None, "PNG IHDR missing")
    row_length = dimensions[0] * channels
    decoded_length = (row_length + 1) * dimensions[1]
    inflater = zlib.decompressobj()
    try:
        # Cap the inflater *during* decode: a correctly CRC'd PNG may still
        # contain an invalid/oversized compressed pixel stream.
        scanlines = inflater.decompress(b"".join(idat_chunks), decoded_length + 1)
    except zlib.error as exc:
        raise RuntimeError("PNG IDAT zlib decode failed") from exc
    require(len(scanlines) == decoded_length and inflater.eof and
            not inflater.unconsumed_tail and not inflater.unused_data,
            "PNG IDAT decompression produced malformed scanlines")

    digest = hashlib.sha256()
    previous = bytearray(row_length)
    offset = 0
    for _ in range(dimensions[1]):
        filter_mode = scanlines[offset]
        offset += 1
        require(filter_mode <= 4, "PNG uses invalid scanline filter")
        row = bytearray(scanlines[offset:offset + row_length])
        offset += row_length
        if filter_mode:
            for column in range(row_length):
                left = row[column - channels] if column >= channels else 0
                above = previous[column]
                upper_left = previous[column - channels] if column >= channels else 0
                correction = (left if filter_mode == 1 else
                              above if filter_mode == 2 else
                              (left + above) // 2 if filter_mode == 3 else
                              _png_paeth(left, above, upper_left))
                row[column] = (row[column] + correction) & 0xff
        digest.update(row)
        previous = row
    return len(payload), digest.hexdigest()


def resolve_pinned_model_selection(recipe, catalog_files, pinned_digests, load):
    """Mirror native recipe defaults and slot normalization BEFORE /models/load.

    This contract deliberately supports one explicitly configured model root.
    It does not simulate inference or silently accept external model sources.
    """
    slots = recipe.get("slots")
    defaults = recipe.get("models", {})
    require(isinstance(slots, list) and slots, "selected recipe has no model slots")
    require(isinstance(defaults, dict), "recipe models defaults must be an object")
    require(isinstance(catalog_files, list), "model catalog files must be an array")

    declared = {slot["id"]: slot for slot in slots}
    require(len(declared) == len(slots), "duplicate native recipe model slots")
    selected = dict(load["models"])
    for key, value in defaults.items():
        if key in selected:
            continue
        if key in ("tae", "taesd") and ("tae" in selected or "taesd" in selected):
            continue
        selected[key] = value

    # prepare_model_load() treats TAE spellings as one native model slot.
    for slot in slots:
        slot_id = slot["id"]
        if slot_id in ("tae", "taesd"):
            alias = "taesd" if slot_id == "tae" else "tae"
            if alias in selected:
                require(slot_id not in selected,
                        "tae and taesd must not both be selected")
                selected[slot_id] = selected.pop(alias)

    by_id = {item["id"]: item for item in catalog_files}
    require(len(by_id) == len(catalog_files), "model catalog has duplicate file IDs")
    result = {}
    for key, model_id in selected.items():
        require(key in declared, "unknown recipe model slot: " + key)
        require(isinstance(model_id, str), "model ID must be a string: " + key)
        if not model_id:
            continue
        item = by_id.get(model_id)
        require(item is not None, "selected model ID not in catalog: " + model_id)
        relative = item.get("relative_path")
        # One model_root is configured by this harness; the native catalog
        # prefixes other roots with their root ID. Never accidentally pin a
        # same-named file from another root.
        require(model_id == relative,
                "qualification only supports the configured primary model root")
        require(relative in pinned_digests,
                "model component has no pinned SHA-256 before load: " + relative)
        categories = declared[key].get("categories",
                                       [declared[key].get("category")])
        require(item.get("category") in categories,
                "model category incompatible with slot: " + key)
        result[key] = model_id

    for slot in slots:
        if slot.get("required"):
            require(slot["id"] in result,
                    "required recipe model slot was not selected: " + slot["id"])
    for group in recipe.get("required_any", []):
        require(any(key in result for key in group),
                "required_any recipe group is not selected: " + repr(group))
    require(result, "no model files selected for native load")
    return result


def require_model_catalog(port, files, load):
    code, catalog = api(port, "GET", "/sdcpp/v1/model-files")
    require(code == 200, "model file catalog not accessible")
    code, recipes = api(port, "GET", "/sdcpp/v1/recipes")
    require(code == 200, "recipe catalog not accessible")
    recipe = next((item for item in recipes.get("recipes", [])
                   if item.get("id") == load["recipe"]), None)
    require(recipe is not None, "selected recipe not in the live model catalog")
    selection = resolve_pinned_model_selection(
        recipe, catalog.get("files", []), files, load)
    print("Pinned all selected components BEFORE native model load:",
          json.dumps(selection, sort_keys=True), flush=True)
    return selection


def qualify(executable, manifest):
    root, files, load, generation, cancel_steps = read_manifest(manifest)
    max_wait = int(os.getenv("SD_APP_WORKFLOW_QUALIFICATION_TIMEOUT", "600"))
    require(60 <= max_wait <= 3600, "qualification timeout must be 60-3600 seconds")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(executable), "--listen-ip", "127.0.0.1", "--listen-port", str(port),
             "--model-dir", str(root)],
            cwd=str(executable.parent), stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT)
        try:
            code, empty = wait_ready(port, process, time.monotonic() + 30)
            require(code == 200 and empty.get("state") == "empty",
                    "service must start with an empty model session")
            require(isinstance(empty.get("instance"), str) and len(empty["instance"]) == 32,
                    "missing native service instance identity")
            expected_selection = require_model_catalog(port, files, load)

            code, loaded = api(port, "POST", "/sdcpp/v1/models/load", load, timeout=max_wait)
            require(code == 200 and loaded.get("state") == "ready",
                    "real /models/load failed: " + repr(loaded))
            require(loaded.get("recipe") == load["recipe"] and
                    loaded.get("instance") == empty["instance"],
                    "loaded model identity differs from the requested session")
            # A native load must resolve the very same set pinned in preflight.
            # Never load first and only later reject implicit recipe components.
            require(loaded.get("models") == expected_selection,
                    "loaded components differ from preflight pinned selection")

            plan = make_plan(loaded, generation)
            stale = json.loads(json.dumps(plan))
            stale["model"]["instance"] = "0" * 32 if loaded["instance"] != "0" * 32 else "1" * 32
            code, error = api(port, "POST", "/sdcpp/v1/workflows", stale)
            require(code == 409 and error.get("code") == "model_binding_mismatch",
                    "stale instance workflow was admitted")
            stale = json.loads(json.dumps(plan))
            stale["model"]["revision"] += 1
            code, error = api(port, "POST", "/sdcpp/v1/workflows", stale)
            require(code == 409 and error.get("code") == "model_binding_mismatch",
                    "stale revision workflow was admitted")

            code, receipt = api(port, "POST", "/sdcpp/v1/workflows", plan)
            require(code == 202 and receipt.get("kind") == "workflow",
                    "real workflow was not admitted: " + repr(receipt))
            job_id = receipt["id"]
            code, pending = api(port, "GET", "/sdcpp/v1/jobs/" + job_id)
            require(code == 200, "first admitted job not visible")
            if pending.get("status") not in TERMINAL:
                code, blocked = api(port, "POST", "/sdcpp/v1/models/unload", {})
                require(code == 409 and blocked.get("code") == "model_busy",
                        "model switched while workflow was pending")
            else:
                raise RuntimeError("generation finished before model-switch race was observable; "
                                   "increase generation.steps for this qualification")

            completed = wait_job(port, job_id, time.monotonic() + max_wait)
            require(completed.get("status") == "completed", "native image workflow failed: " + repr(completed))
            states = {n["id"]: n["status"] for n in completed.get("nodes", [])}
            require(states == {"source": "completed", "final": "completed"},
                    "unexpected DAG node state after generation")
            result = completed.get("result", {})
            require(result.get("model") == plan["model"], "terminal workflow lost its model binding")
            require(len(result.get("outputs", [])) == 1 and
                    result["outputs"][0].get("node_id") == "final",
                    "only the named output must be retained")
            images = result["outputs"][0].get("images", [])
            require(len(images) == 1 and "generation_info" in images[0],
                    "real result/metadata ownership not preserved")
            size, decoded_digest = check_png(
                images[0]["b64_json"], generation["width"], generation["height"])
            print("Native PNG pixels decoded:", size, "bytes; SHA-256:", decoded_digest,
                  flush=True)

            code, error = api(port, "GET", "/sdcpp/v1/jobs/" + job_id,
                              extra_headers={"Origin": "https://evil.example"})
            require(code == 403 and error.get("code") == "untrusted_origin",
                    "cross-origin read of a workflow result was allowed")
            code, error = api(port, "POST", "/sdcpp/v1/jobs/" + job_id + "/cancel", {},
                              extra_headers={"Origin": "https://evil.example"})
            require(code == 403 and error.get("code") == "untrusted_origin",
                    "cross-origin cancellation of a workflow was allowed")

            code, terminal_cancel = api(port, "POST", "/sdcpp/v1/jobs/" + job_id + "/cancel", {})
            require(code == 200 and terminal_cancel.get("status") == "completed",
                    "cancel after success changed immutable terminal state")

            long_params = dict(generation)
            long_params["steps"] = cancel_steps
            code, first = api(port, "POST", "/sdcpp/v1/workflows",
                              make_plan(loaded, long_params))
            require(code == 202, "running-cancel job admission failed: " + repr(first))
            first_id = first["id"]
            active = None
            start_deadline = time.monotonic() + min(max_wait, 90)
            while time.monotonic() < start_deadline:
                code, state = api(port, "GET", "/sdcpp/v1/jobs/" + first_id)
                require(code == 200, "running job disappeared")
                if state["status"] == "generating" and any(
                        node["status"] == "running" for node in state.get("nodes", [])):
                    active = state
                    break
                require(state["status"] not in TERMINAL, "could not observe running native generation")
                time.sleep(0.05)
            require(active is not None, "running generation was never observed")

            code, second = api(port, "POST", "/sdcpp/v1/workflows",
                               make_plan(loaded, generation, two=False))
            require(code == 202, "queued-cancel job admission failed: " + repr(second))
            second_id = second["id"]
            code, queued = api(port, "GET", "/sdcpp/v1/jobs/" + second_id)
            require(code == 200 and queued["status"] == "queued",
                    "second job did not remain queued during active generation")
            code, cancellation = api(port, "POST",
                                     "/sdcpp/v1/jobs/" + second_id + "/cancel", {})
            require(code == 200 and cancellation["status"] == "cancelled" and
                    cancellation["result"] is None and
                    all(node["status"] == "skipped" for node in cancellation["nodes"]),
                    "queued cancellation did not clean its node state")

            code, cancellation = api(port, "POST",
                                     "/sdcpp/v1/jobs/" + first_id + "/cancel", {})
            require(code == 200 and cancellation.get("cancel_requested") and
                    cancellation["status"] == "generating",
                    "running cancellation was not acknowledged while executing")
            cancelled = wait_job(port, first_id, time.monotonic() + max_wait)
            require(cancelled["status"] == "cancelled" and cancelled["result"] is None,
                    "running native generation failed to cancel and release output")
            code, queued = api(port, "GET", "/sdcpp/v1/jobs/" + second_id)
            require(code == 200 and queued["status"] == "cancelled",
                    "queued cancellation changed after the other job retired")
            print("Queued and running cancellation / result cleanup verified", flush=True)

            code, unloaded = api(port, "POST", "/sdcpp/v1/models/unload", {})
            require(code == 200 and unloaded["state"] == "empty", "model unload after jobs failed")
            code, reloaded = api(port, "POST", "/sdcpp/v1/models/load", load, timeout=max_wait)
            require(code == 200 and reloaded.get("state") == "ready" and
                    reloaded.get("instance") == loaded["instance"] and
                    reloaded.get("revision") != loaded.get("revision"),
                    "reload did not advance model revision on the same process")
            code, error = api(port, "POST", "/sdcpp/v1/workflows", plan)
            require(code == 409 and error.get("code") == "model_binding_mismatch",
                    "old workflow accepted after model reload")
            print("Model unload, reload and stale-workflow rejection verified", flush=True)
            print("PASS: actual sd.cpp model weights, native DAG, PNG, cancellation and model binding",
                  flush=True)
        except Exception:
            log.flush()
            log.seek(0, 2)
            log.seek(max(0, log.tell() - 16 * 1024))
            sys.stderr.write(log.read().decode("utf-8", errors="replace"))
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


def self_test():
    # The CI self-test only validates this harness, not inference or weights.
    with tempfile.TemporaryDirectory(prefix="sd-workflow-harness-") as directory:
        root = Path(directory)
        weights = root / "checkpoints"
        weights.mkdir()
        sample = weights / "not-a-model.bin"
        sample.write_bytes(b"harness fixture -- never passed to sd.cpp")
        manifest = {"version": 1, "model_root": str(root.resolve()),
                    "sha256": {"checkpoints/not-a-model.bin":
                               hashlib.sha256(sample.read_bytes()).hexdigest()},
                    "load": {"recipe": "dummy", "models": {"model": "checkpoints/not-a-model.bin"}},
                    "generation": {"prompt": "self test", "width": 64, "height": 64, "steps": 1},
                    "cancel_steps": 10}
        path = root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        parsed = read_manifest(path)
        require(parsed[2]["recipe"] == "dummy", "manifest self-test failed")
        manifest["sha256"]["checkpoints/not-a-model.bin"] = "0" * 64
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            read_manifest(path)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("SHA-256 mismatch was not rejected")

        # Exercise native recipe default merging without loading any weights.
        catalog = [{"id": "checkpoints/not-a-model.bin",
                    "relative_path": "checkpoints/not-a-model.bin",
                    "category": "checkpoints"}]
        recipe = {"id": "dummy", "slots": [{"id": "model", "required": True,
                                           "categories": ["checkpoints"]}],
                  "models": {"model": "checkpoints/not-a-model.bin"}}
        defaults_only = {"recipe": "dummy", "models": {}}
        pinned = {"checkpoints/not-a-model.bin": "0" * 64}
        selection = resolve_pinned_model_selection(recipe, catalog, pinned, defaults_only)
        require(selection == {"model": "checkpoints/not-a-model.bin"},
                "implicit recipe model default not resolved")
        try:
            resolve_pinned_model_selection(recipe, catalog, {}, defaults_only)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("unverified implicit recipe component was allowed before load")
        try:
            resolve_pinned_model_selection(recipe, catalog, pinned,
                                           {"recipe": "dummy", "models":
                                            {"model": "checkpoints/unknown.safetensors"}})
        except RuntimeError:
            pass
        else:
            raise RuntimeError("unknown explicitly selected model was allowed")

        catalog.append({"id": "vae/tae.safetensors", "relative_path": "vae/tae.safetensors",
                        "category": "vae"})
        tae_recipe = {"id": "dummy", "slots": [
            {"id": "model", "required": True, "categories": ["checkpoints"]},
            {"id": "tae", "required": False, "categories": ["vae"]}],
            "models": {"model": "checkpoints/not-a-model.bin",
                       "taesd": "vae/tae.safetensors"}}
        both_pinned = dict(pinned, **{"vae/tae.safetensors": "1" * 64})
        selection = resolve_pinned_model_selection(
            tae_recipe, catalog, both_pinned, defaults_only)
        require(selection == {"model": "checkpoints/not-a-model.bin",
                              "tae": "vae/tae.safetensors"}, "TAE alias not normalized")
        try:
            resolve_pinned_model_selection(
                tae_recipe, catalog, pinned, defaults_only)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("unverified TAE default escaped native preflight")

        image = bytes([0, 255, 0, 0])
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + \
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
        ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + \
            chunk(b"IDAT", zlib.compress(image)) + chunk(b"IEND", b"")
        size, digest = check_png(base64.b64encode(png), 1, 1)
        require(size == len(png) and digest == hashlib.sha256(image[1:]).hexdigest(),
                "PNG pixel decode self-test failed")

        def assert_bad_png(value, message):
            try:
                check_png(base64.b64encode(value), 1, 1)
            except RuntimeError:
                pass
            else:
                raise RuntimeError(message)

        broken = bytearray(png)
        broken[-5] ^= 1
        assert_bad_png(broken, "corrupt PNG CRC was accepted")
        # These next cases have freshly computed *correct* CRCs, unlike the
        # previous case. They must still fail because their pixels are invalid.
        def image_png(raw_idat, depth=8, interlace=0, width=1, height=1):
            header = struct.pack(">IIBBBBB", width, height, depth, 2, 0, 0, interlace)
            return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                    chunk(b"IDAT", raw_idat) + chunk(b"IEND", b""))

        assert_bad_png(image_png(zlib.compress(b"\xff\xff\x00\x00")),
                       "valid CRC with invalid PNG filter was accepted")
        assert_bad_png(image_png(b"not zlib data"),
                       "valid CRC with corrupt zlib stream was accepted")
        assert_bad_png(image_png(zlib.compress(b"\x00\xff\x00")),
                       "valid CRC with truncated pixels was accepted")
        assert_bad_png(image_png(zlib.compress(b"\x00" * 1024)),
                       "valid CRC with decompression bomb was accepted")
        assert_bad_png(image_png(zlib.compress(image), interlace=1),
                       "unsupported PNG interlace was accepted")
        for filter_mode in range(5):
            candidate = image_png(zlib.compress(bytes([filter_mode, 255, 0, 0])))
            _, decoded = check_png(base64.b64encode(candidate), 1, 1)
            require(decoded == hashlib.sha256(b"\xff\x00\x00").hexdigest(),
                    "PNG scanline filter failed to decode real RGB pixels")
        two_red_pixels = hashlib.sha256(b"\xff\x00\x00" * 2).hexdigest()
        # Filter 1 (Sub) uses the already reconstructed left pixel.
        sub_scanline = b"\x01\xff\x00\x00\x00\x00\x00"
        _, decoded = check_png(base64.b64encode(
            image_png(zlib.compress(sub_scanline), width=2)), 2, 1)
        require(decoded == two_red_pixels, "PNG Sub predictor did not restore the second pixel")
        # Filter 2 (Up) uses bytes from the previous reconstructed scanline.
        up_scanlines = b"\x00\xff\x00\x00\x02\x00\x00\x00"
        _, decoded = check_png(base64.b64encode(
            image_png(zlib.compress(up_scanlines), height=2)), 1, 2)
        require(decoded == two_red_pixels, "PNG Up predictor did not restore the second row")
    print("PASS: pre-load SHA-256/recipe selection and bounded real PNG pixel decode; no inference")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        self_test()
    elif len(sys.argv) == 3:
        binary = Path(sys.argv[1]).resolve(strict=True)
        config = Path(sys.argv[2]).resolve(strict=True)
        require(binary.is_file(), "missing sd-app-server executable")
        qualify(binary, config)
    else:
        raise SystemExit("usage: workflow_real_model.py --self-test | /path/sd-app-server /path/manifest.json")
