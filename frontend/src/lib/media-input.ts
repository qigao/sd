import { onBeforeUnmount, watch, type Ref } from "vue";
import { encodeAudioReference, extractVideoFrames, MAX_MEDIA_BYTES, readAudioDuration } from "./media";
import type { MediaReference } from "./types";

export function useMediaInput(kind: "video" | "audio", entry: Ref<MediaReference | null>) {
    let controller: AbortController | null = null;

    watch(entry, (_value, previous) => {
        controller?.abort();
        if (previous) URL.revokeObjectURL(previous.url);
    }, { flush: "sync" });

    function clear(): void {
        entry.value = null;
    }

    async function select(files: FileList): Promise<void> {
        if (!files.length) return;
        const file = files[0];
        entry.value = {
            file, url: URL.createObjectURL(file), duration: 0, frames: [], audio: "", useAudio: false,
            processing: true, progress: "Loading file…", error: "", audioError: "",
        };
        const current = entry.value;
        controller = new AbortController();
        const signal = controller.signal;
        try {
            if (file.size > MAX_MEDIA_BYTES) throw new Error("Use a file smaller than 256 MiB.");
            if (kind === "video") {
                const result = await extractVideoFrames(current.url, signal, (progress) => { current.progress = progress; });
                if (signal.aborted) return;
                current.frames = result.frames;
                current.duration = result.duration;
            } else {
                current.duration = await readAudioDuration(current.url, signal);
                current.progress = "Decoding audio…";
                const audio = await encodeAudioReference(file, signal);
                if (signal.aborted) return;
                current.audio = audio;
            }
        } catch (error) {
            if (!signal.aborted) {
                const suggestion = kind === "video" ? "Try an H.264 MP4 video." : "Try a WAV or MP3 audio file.";
                current.error = `${error instanceof Error ? error.message : String(error)} ${suggestion}`;
            }
        } finally {
            if (!signal.aborted) current.processing = false;
        }
    }

    async function setUseAudio(enabled: boolean): Promise<void> {
        const current = entry.value;
        if (!current?.frames.length) return;
        controller?.abort();
        current.useAudio = enabled;
        current.processing = false;
        current.audioError = "";
        if (!enabled || current.audio) return;
        controller = new AbortController();
        const signal = controller.signal;
        current.processing = true;
        current.progress = "Decoding video soundtrack…";
        try {
            const audio = await encodeAudioReference(current.file, signal, current.duration);
            if (!signal.aborted) current.audio = audio;
        } catch (error) {
            if (!signal.aborted) current.audioError = `Could not decode the video soundtrack. ${error instanceof Error ? error.message : String(error)} Disable “Use video soundtrack” to continue without it.`;
        } finally {
            if (!signal.aborted) current.processing = false;
        }
    }

    onBeforeUnmount(clear);
    return { select, clear, setUseAudio };
}
