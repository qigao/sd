import { computed, ref } from "vue";
import type { Ref } from "vue";
import type { GenerationForm, VideoConstraints } from "./types";

export type VideoLengthMode = "frames" | "duration";

export function alignVideoFrames(frames: number, constraints: VideoConstraints = {}): number {
    const step = constraints.frame_step ?? 1;
    const offset = constraints.frame_offset ?? 0;
    const first = offset + Math.max(0, Math.ceil(((constraints.min_frames ?? 1) - offset) / step)) * step;
    const last = offset + Math.floor(((constraints.max_frames ?? 4096) - offset) / step) * step;
    const units = (frames - offset) / step;
    // Stabilize duration -> frames round trips at exact frame boundaries.
    const rounded = constraints.frame_rounding === "ceil" ? Math.ceil(units - 1e-9) : Math.floor(units + 1e-9);
    return Math.min(last, Math.max(first, rounded * step + offset));
}

export function useVideoLength(form: GenerationForm, constraints: Ref<VideoConstraints | undefined>) {
    const mode = ref<VideoLengthMode>("frames");
    const duration = ref<number | string>(form.video_frames / form.fps);
    const fps = computed({
        get: () => constraints.value?.fixed_fps ?? form.fps,
        set: (value: number) => { form.fps = value; },
    });
    const requestedFrames = computed(() => mode.value === "duration" ? Number(duration.value) * fps.value : form.video_frames);
    const error = computed(() => {
        if (!Number.isInteger(fps.value) || fps.value < 1 || fps.value > 240) return "FPS must be an integer from 1 to 240.";
        if (mode.value === "duration" && (!Number.isFinite(Number(duration.value)) || Number(duration.value) <= 0)) return "Enter a duration greater than zero.";
        if (mode.value === "frames" && (!Number.isInteger(form.video_frames) || form.video_frames < 1)) return "Enter a whole number of frames greater than zero.";
        if (!Number.isFinite(requestedFrames.value) || requestedFrames.value > 4096) return "The requested length must not exceed 4096 frames.";
        return "";
    });
    const frames = computed(() => error.value ? 0 : alignVideoFrames(requestedFrames.value, constraints.value));
    const seconds = computed(() => frames.value / fps.value);
    const summary = computed(() => error.value ? "Set video length" : `${mode.value === "frames" ? `${frames.value}f` : `≈${Number(seconds.value.toFixed(2))}s`} · ${fps.value} fps`);
    const details = computed(() => error.value || `${frames.value} frames · ${fps.value} FPS · ≈${Number(seconds.value.toFixed(3))} seconds${mode.value === "duration" ? ` (requested ${duration.value}s)` : ""}`);

    function resetDuration(): void {
        duration.value = alignVideoFrames(form.video_frames, constraints.value) / fps.value;
    }

    function setMode(next: VideoLengthMode): void {
        if (next === mode.value || error.value) return;
        if (next === "duration") duration.value = seconds.value;
        else form.video_frames = frames.value;
        mode.value = next;
    }

    return { mode, duration, fps, frames, seconds, error, summary, details, resetDuration, setMode };
}
