import type { GenerationFeatures, GenerationMode } from "./types";

export function featuresForMode(features: GenerationFeatures, mode: GenerationMode): GenerationFeatures {
    const result = { ...features };
    if (mode === "image") {
        result.init_image = Boolean(result.img2img);
        for (const key of ["end_image", "control_frames", "vace", "high_noise", "ref_videos", "ref_audios", "ref_video_audios"]) result[key] = false;
    } else {
        for (const key of ["img2img", "mask_image", "controlnet", "control_image", "ip_adapter", "shifted_timestep"]) result[key] = false;
    }
    result.mask_image = Boolean(result.mask_image && result.img2img);
    result.control_image = Boolean(result.controlnet || result.control_image);
    result.ip_adapter_image = Boolean(result.ip_adapter);
    return result;
}

export function supportsEta(method: string, features: GenerationFeatures): boolean {
    if (!method || method === "default") method = features.cache_unet ? "euler_a" : "euler";
    return ["euler_a", "dpm++2s_a", "res_multistep", "res_2s", "er_sde", "dpm++2m_sde", "dpm++2m_sde_bt", "ddim_trailing", "tcd", "euler_a_cfg_pp", "euler_ge"].includes(method);
}

export function supportsFlowShift(scheduler: string, features: GenerationFeatures): boolean {
    return Boolean(features.flow_shift) && !["flux", "flux2", "ltx2", "llada_image"].includes(scheduler);
}

export function availableCacheModes(features: GenerationFeatures): string[] {
    if (!features.cache) return ["disabled"];
    return ["disabled", ...(features.cache_unet ? ["ucache"] : []),
        ...(features.cache_dit ? ["easycache", "dbcache", "taylorseer", "cache-dit"] : []), "spectrum"];
}

export function usesSCM(mode: string): boolean {
    return ["dbcache", "taylorseer", "cache-dit"].includes(mode);
}
