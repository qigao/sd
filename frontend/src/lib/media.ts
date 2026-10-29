export const REFERENCE_VIDEO_FPS = 24;
export const MAX_VIDEO_SECONDS = 30;
export const MAX_AUDIO_SECONDS = 120;
export const MAX_MEDIA_BYTES = 256 * 1024 * 1024;
const MAX_FRAME_DATA_LENGTH = 64 * 1024 * 1024;
const AUDIO_SAMPLE_RATE = 32000;

function checkAbort(signal: AbortSignal): void {
    if (signal.aborted) throw new DOMException("Media processing cancelled.", "AbortError");
}

function waitForMedia(media: HTMLMediaElement, event: string, signal: AbortSignal, start: () => void): Promise<void> {
    checkAbort(signal);
    return new Promise((resolve, reject) => {
        const cleanup = () => {
            clearTimeout(timer);
            media.removeEventListener(event, ready);
            media.removeEventListener("error", failed);
            signal.removeEventListener("abort", aborted);
        };
        const ready = () => { cleanup(); resolve(); };
        const failed = () => { cleanup(); reject(new Error("The browser could not decode this file.")); };
        const aborted = () => { cleanup(); reject(new DOMException("Media processing cancelled.", "AbortError")); };
        const timer = window.setTimeout(() => {
            cleanup();
            reject(new Error("Media decoding timed out. Try a shorter clip or a different encoding."));
        }, 30000);
        media.addEventListener(event, ready, { once: true });
        media.addEventListener("error", failed, { once: true });
        signal.addEventListener("abort", aborted, { once: true });
        try { start(); } catch (error) { cleanup(); reject(error); }
    });
}

function releaseMedia(media: HTMLMediaElement): void {
    media.pause();
    media.removeAttribute("src");
    media.load();
}

function validateDuration(duration: number, maximum: number): void {
    if (!Number.isFinite(duration) || duration <= 0) throw new Error("Could not read the file duration.");
    if (duration > maximum) throw new Error(`Use a clip no longer than ${maximum} seconds.`);
}

export async function readAudioDuration(url: string, signal: AbortSignal): Promise<number> {
    const audio = document.createElement("audio");
    audio.preload = "metadata";
    try {
        await waitForMedia(audio, "loadedmetadata", signal, () => { audio.src = url; });
        validateDuration(audio.duration, MAX_AUDIO_SECONDS);
        return audio.duration;
    } finally {
        releaseMedia(audio);
    }
}

export async function extractVideoFrames(url: string, signal: AbortSignal, progress: (value: string) => void): Promise<{ frames: string[]; duration: number }> {
    const video = document.createElement("video");
    video.muted = true;
    video.playsInline = true;
    video.preload = "auto";
    const canvas = document.createElement("canvas");
    try {
        await waitForMedia(video, "loadeddata", signal, () => { video.src = url; });
        validateDuration(video.duration, MAX_VIDEO_SECONDS);
        if (!video.videoWidth || !video.videoHeight) throw new Error("The file has no decodable video track.");
        const count = Math.floor(video.duration * REFERENCE_VIDEO_FPS);
        if (count < 5) throw new Error("Use a video at least 5/24 seconds long.");
        const scale = Math.min(1, 1280 / Math.max(video.videoWidth, video.videoHeight),
            Math.sqrt((768 * 1344) / (video.videoWidth * video.videoHeight)));
        canvas.width = Math.max(1, Math.round(video.videoWidth * scale));
        canvas.height = Math.max(1, Math.round(video.videoHeight * scale));
        const context = canvas.getContext("2d");
        if (!context) throw new Error("Could not create a canvas for video frames.");
        const frames: string[] = [];
        let size = 0;
        for (let i = 0; i < count; i++) {
            checkAbort(signal);
            if (i > 0) {
                await waitForMedia(video, "seeked", signal, () => { video.currentTime = i / REFERENCE_VIDEO_FPS; });
            }
            context.drawImage(video, 0, 0, canvas.width, canvas.height);
            const frame = canvas.toDataURL("image/jpeg", 0.9);
            size += frame.length;
            if (size > MAX_FRAME_DATA_LENGTH) throw new Error("Decoded video is too large. Use a shorter clip.");
            frames.push(frame);
            progress(`Preparing video: ${i + 1}/${count} frames`);
        }
        return { frames, duration: count / REFERENCE_VIDEO_FPS };
    } finally {
        releaseMedia(video);
        canvas.width = canvas.height = 0;
    }
}

function readFile(file: Blob, signal: AbortSignal, asDataUrl: true): Promise<string>;
function readFile(file: Blob, signal: AbortSignal, asDataUrl: false): Promise<ArrayBuffer>;
function readFile(file: Blob, signal: AbortSignal, asDataUrl: boolean): Promise<string | ArrayBuffer> {
    checkAbort(signal);
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        const abort = () => reader.abort();
        signal.addEventListener("abort", abort, { once: true });
        reader.onloadend = () => signal.removeEventListener("abort", abort);
        reader.onload = () => resolve(reader.result as string | ArrayBuffer);
        reader.onerror = () => reject(reader.error || new Error("Could not read the file."));
        reader.onabort = () => reject(new DOMException("Media processing cancelled.", "AbortError"));
        if (asDataUrl) reader.readAsDataURL(file);
        else reader.readAsArrayBuffer(file);
    });
}

function decodeAudio(context: AudioContext, bytes: ArrayBuffer, signal: AbortSignal): Promise<AudioBuffer> {
    checkAbort(signal);
    return new Promise((resolve, reject) => {
        const cleanup = () => { clearTimeout(timer); signal.removeEventListener("abort", abort); };
        const abort = () => { cleanup(); reject(new DOMException("Media processing cancelled.", "AbortError")); };
        const timer = window.setTimeout(() => { cleanup(); reject(new Error("Audio decoding timed out.")); }, 30000);
        signal.addEventListener("abort", abort, { once: true });
        // decodeAudioData has no cancellation API; discard its result after cancellation.
        context.decodeAudioData(bytes).then((buffer) => { cleanup(); resolve(buffer); }, (error) => { cleanup(); reject(error); });
    });
}

export async function encodeAudioReference(file: File, signal: AbortSignal, duration?: number): Promise<string> {
    const context = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });
    try {
        const bytes = await readFile(file, signal, false);
        const buffer = await decodeAudio(context, bytes, signal);
        checkAbort(signal);
        validateDuration(buffer.duration, MAX_AUDIO_SECONDS);
        if (buffer.numberOfChannels > 2) throw new Error("Use a mono or stereo audio track.");
        const channels = buffer.numberOfChannels;
        const count = Math.min(buffer.length, Math.floor((duration ?? buffer.duration) * buffer.sampleRate));
        if (count < 1) throw new Error("The file has no audio samples.");
        const data = new ArrayBuffer(44 + count * channels * 2);
        const view = new DataView(data);
        const text = (offset: number, value: string) => {
            for (let i = 0; i < value.length; i++) view.setUint8(offset + i, value.charCodeAt(i));
        };
        text(0, "RIFF");
        view.setUint32(4, data.byteLength - 8, true);
        text(8, "WAVE");
        text(12, "fmt ");
        view.setUint32(16, 16, true);
        view.setUint16(20, 1, true);
        view.setUint16(22, channels, true);
        view.setUint32(24, buffer.sampleRate, true);
        view.setUint32(28, buffer.sampleRate * channels * 2, true);
        view.setUint16(32, channels * 2, true);
        view.setUint16(34, 16, true);
        text(36, "data");
        view.setUint32(40, data.byteLength - 44, true);
        const samples = Array.from({ length: channels }, (_, channel) => buffer.getChannelData(channel));
        for (let start = 0; start < count; start += 16384) {
            checkAbort(signal);
            for (let i = start; i < Math.min(count, start + 16384); i++) {
                for (let c = 0; c < channels; c++) {
                    const value = Math.max(-1, Math.min(1, samples[c][i]));
                    view.setInt16(44 + (i * channels + c) * 2, Math.round(value * (value < 0 ? 32768 : 32767)), true);
                }
            }
            await new Promise<void>((resolve) => setTimeout(resolve, 0));
        }
        return await readFile(new Blob([data], { type: "audio/wav" }), signal, true);
    } finally {
        await context.close();
    }
}
