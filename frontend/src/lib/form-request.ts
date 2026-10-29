import { availableCacheModes, featuresForMode, supportsEta, supportsFlowShift, usesSCM } from "./generation-features";
import type { GenerationFeatures, GenerationForm, GenerationMode, SampleParams } from "./types";

function parseNumber(value: unknown, fallback: number): number {
    if (value === "" || value == null) {
        return fallback;
    }
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : fallback;
}

function nullable(value: unknown): unknown {
    return value === "" || value == null ? null : value;
}

interface SampleParamsRequest {
    sample_steps: number;
    shifted_timestep: number;
    guidance: {
        txt_cfg: number;
        distilled_guidance: number;
        slg: {
            layers: number[];
            layer_start: number;
            layer_end: number;
            scale: number;
        };
        img_cfg?: number;
    };
    eta?: number;
    flow_shift?: number;
    scheduler?: string;
    sample_method?: string;
}

function buildSampleParams(sample: SampleParams, defaultSteps: number): SampleParamsRequest {
    const eta = nullable(sample.eta);
    const flowShift = nullable(sample.flow_shift);
    const imgCfg = nullable(sample.guidance.img_cfg);

    const sampleParams: SampleParamsRequest = {
        sample_steps: parseNumber(sample.sample_steps, defaultSteps),
        shifted_timestep: parseNumber(sample.shifted_timestep, 0),
        guidance: {
            txt_cfg: parseNumber(sample.guidance.txt_cfg, 7),
            distilled_guidance: parseNumber(sample.guidance.distilled_guidance, 3.5),
            slg: {
                layers: String(sample.guidance.slg_layers || "")
                    .split(",")
                    .map((item) => Number(item.trim()))
                    .filter((value) => Number.isInteger(value)),
                layer_start: parseNumber(sample.guidance.layer_start, 0.01),
                layer_end: parseNumber(sample.guidance.layer_end, 0.2),
                scale: parseNumber(sample.guidance.scale, 0),
            },
        },
    };

    if (eta != null) {
        sampleParams.eta = Number(eta);
    }
    if (flowShift != null) {
        sampleParams.flow_shift = Number(flowShift);
    }
    if (imgCfg != null) {
        sampleParams.guidance.img_cfg = Number(imgCfg);
    }

    const scheduler =
        sample.scheduler && sample.scheduler !== "default"
            ? sample.scheduler
            : undefined;
    const sampleMethod =
        sample.sample_method && sample.sample_method !== "default"
            ? sample.sample_method
            : undefined;

    if (scheduler) {
        sampleParams.scheduler = scheduler;
    }
    if (sampleMethod) {
        sampleParams.sample_method = sampleMethod;
    }

    return sampleParams;
}

function buildLoraRequest(form: GenerationForm) {
    return form.lora
        .filter((item) => String(item.path || "").trim())
        .map((item) => ({
            path: String(item.path).trim(),
            multiplier: parseNumber(item.multiplier, 1.0),
            is_high_noise: Boolean(item.is_high_noise),
        }));
}

function buildTilingRequest(form: GenerationForm) {
    return {
        enabled: Boolean(form.vae_tiling_params.enabled),
        tile_size_w: parseNumber(form.vae_tiling_params.tile_size_w, 0),
        tile_size_h: parseNumber(form.vae_tiling_params.tile_size_h, 0),
        target_overlap: parseNumber(form.vae_tiling_params.target_overlap, 0.5),
        rel_size_w: parseNumber(form.vae_tiling_params.rel_size_w, 0),
        rel_size_h: parseNumber(form.vae_tiling_params.rel_size_h, 0),
    };
}

export function buildRequestBody(form: GenerationForm) {
    const request = {
        prompt: String(form.prompt || "").trim(),
        negative_prompt: form.negative_prompt,
        clip_skip: parseNumber(form.clip_skip, -1),
        width: parseNumber(form.width, 512),
        height: parseNumber(form.height, 512),
        strength: parseNumber(form.strength, 0.75),
        seed: parseNumber(form.seed, -1),
        sample_params: buildSampleParams(form.sample_params, 20),
        lora: buildLoraRequest(form),
        vae_tiling_params: buildTilingRequest(form),
        cache_mode: form.cache.mode || "disabled",
        cache_option: String(form.cache.option || ""),
        scm_mask: String(form.cache.scm_mask || ""),
        scm_policy_dynamic: Boolean(form.cache.scm_policy_dynamic),
        output_format: form.output_format,
        output_compression: parseNumber(form.output_compression, 100),
        preview: form.preview_mode || "none",
        preview_interval: parseNumber(form.preview_interval, 1),
    };

    if (!request.prompt) {
        throw new Error("prompt is required");
    }

    return request;
}

export function buildImageRequestBody(form: GenerationForm) {
    return {
        ...buildRequestBody(form),
        batch_count: parseNumber(form.batch_count, 1),
        auto_resize_ref_image: true,
        increase_ref_index: false,
        control_strength: parseNumber(form.control_strength, 0.9),
        ip_adapter_strength: parseNumber(form.ip_adapter_strength, 1.0),
        ip_adapter_image: form.ip_adapter_image ? form.ip_adapter_image.dataUrl : null,
        init_image: form.init_image ? form.init_image.dataUrl : null,
        ref_images: form.ref_images.map((item) => item.dataUrl),
        mask_image: form.mask_image ? form.mask_image.dataUrl : null,
        control_image: form.control_image ? form.control_image.dataUrl : null,
    };
}

export function buildVideoRequestBody(form: GenerationForm) {
    if ((form.ref_images.length || form.ref_video) && (form.init_image || form.end_image)) {
        throw new Error("Reference images/videos cannot be combined with an initial or end frame. Clear the references or both keyframes before generating.");
    }
    for (const entry of [form.ref_video, form.ref_audio]) {
        if (!entry) continue;
        if (entry.processing) throw new Error("Wait for reference media processing to finish.");
        if (entry.error) throw new Error(entry.error);
    }
    if (form.ref_video && !form.ref_video.frames.length) throw new Error("The reference video has no decoded frames.");
    if (form.ref_audio && !form.ref_audio.audio) throw new Error("The reference audio has not been decoded.");
    if (form.ref_video?.useAudio && !form.ref_video.audio) {
        throw new Error(form.ref_video.audioError || "The video soundtrack has not been decoded.");
    }

    return {
        ...buildRequestBody(form),
        video_frames: parseNumber(form.video_frames, 33),
        fps: parseNumber(form.fps, 16),
        moe_boundary: parseNumber(form.moe_boundary, 0.875),
        vace_strength: parseNumber(form.vace_strength, 1.0),
        init_image: form.init_image ? form.init_image.dataUrl : null,
        end_image: form.end_image ? form.end_image.dataUrl : null,
        control_frames: form.control_frames.map((item) => item.dataUrl),
        ref_images: form.ref_images.map((item) => item.dataUrl),
        ref_videos: form.ref_video ? [form.ref_video.frames] : [],
        ref_audios: form.ref_audio ? [form.ref_audio.audio] : [],
        ref_video_audios: form.ref_video?.useAudio ? [form.ref_video.audio] : [],
        high_noise_sample_params: buildSampleParams(form.high_noise_sample_params, -1),
    };
}

export function buildRequestBodyForMode(mode: GenerationMode, form: GenerationForm, declared?: GenerationFeatures) {
    if (!declared) return mode === "video" ? buildVideoRequestBody(form) : buildImageRequestBody(form);
    const features = featuresForMode(declared, mode);
    const visibleForm = {
        ...form,
        init_image: features.init_image ? form.init_image : null,
        end_image: features.end_image ? form.end_image : null,
        mask_image: features.mask_image ? form.mask_image : null,
        control_image: features.control_image ? form.control_image : null,
        ip_adapter_image: features.ip_adapter ? form.ip_adapter_image : null,
        ref_images: features.ref_images ? form.ref_images : [],
        ref_video: features.ref_videos && form.ref_video
            ? { ...form.ref_video, useAudio: Boolean(features.ref_video_audios && form.ref_video.useAudio) } : null,
        ref_audio: features.ref_audios ? form.ref_audio : null,
        control_frames: features.control_frames ? form.control_frames : [],
    };
    const request: Record<string, any> = mode === "video" ? buildVideoRequestBody(visibleForm) : buildImageRequestBody(visibleForm);
    const fields: Record<string, string[]> = {
        clip_skip: ["clip_skip"], img2img: ["strength"], init_image: ["init_image"],
        mask_image: ["mask_image"], controlnet: ["control_strength"],
        control_image: ["control_image"], ip_adapter: ["ip_adapter_image", "ip_adapter_strength"],
        ref_images: ["ref_images", "auto_resize_ref_image", "increase_ref_index"],
        ref_videos: ["ref_videos"], ref_audios: ["ref_audios"], ref_video_audios: ["ref_video_audios"],
        end_image: ["end_image"], control_frames: ["control_frames"], vace: ["vace_strength"],
        high_noise: ["high_noise_sample_params", "moe_boundary"], lora: ["lora"],
        vae_tiling: ["vae_tiling_params"], cache: ["cache_mode", "cache_option", "scm_mask", "scm_policy_dynamic"],
        preview: ["preview", "preview_interval"],
    };
    for (const [feature, keys] of Object.entries(fields)) {
        if (!features[feature]) for (const key of keys) delete request[key];
    }
    if (!visibleForm.init_image) delete request.strength;
    for (const key of ["sample_params", "high_noise_sample_params"] as const) {
        const sample = request[key];
        if (!sample) continue;
        if (!features.distilled_guidance) delete sample.guidance.distilled_guidance;
        if (!features.img_cfg) delete sample.guidance.img_cfg;
        if (!features.slg) delete sample.guidance.slg;
        if (!features.shifted_timestep) delete sample.shifted_timestep;
        if (!supportsEta(sample.sample_method, features)) delete sample.eta;
        if (!supportsFlowShift(sample.scheduler, features)) delete sample.flow_shift;
    }
    if (request.lora && !features.high_noise) {
        request.lora = request.lora.filter((item: { is_high_noise: boolean }) => !item.is_high_noise)
            .map(({ is_high_noise, ...item }: { is_high_noise: boolean }) => item);
    }
    if (features.cache) {
        if (!availableCacheModes(features).includes(request.cache_mode)) request.cache_mode = "disabled";
        if (request.cache_mode === "disabled") delete request.cache_option;
        if (!usesSCM(request.cache_mode)) {
            delete request.scm_mask;
            delete request.scm_policy_dynamic;
        }
    }
    if (request.preview === "tae" && !features.preview_tae) request.preview = "none";
    if (request.preview === "none") delete request.preview_interval;
    return request;
}
