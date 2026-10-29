<script setup lang="ts">
import { ref } from "vue";
import type { MediaReference } from "../lib/types";
import { MAX_AUDIO_SECONDS, MAX_VIDEO_SECONDS } from "../lib/media";

defineProps<{
    kind: "video" | "audio";
    entry: MediaReference | null;
    allowSoundtrack?: boolean;
}>();
const emit = defineEmits<{
    (e: "select", files: FileList): void;
    (e: "clear"): void;
    (e: "soundtrack", enabled: boolean): void;
}>();
const input = ref<HTMLInputElement | null>(null);
const dragging = ref(false);

function pick(event: Event): void {
    const target = event.target as HTMLInputElement;
    if (target.files?.length) emit("select", target.files);
    target.value = "";
}

function drop(event: DragEvent): void {
    dragging.value = false;
    if (event.dataTransfer?.files.length) emit("select", event.dataTransfer.files);
}
</script>

<template>
    <div class="group media-input">
        <label>{{ kind === "video" ? "Reference Video" : "Reference Audio" }}</label>
        <div class="upload-card" :class="{ 'upload-card--active': dragging }" @dragover.prevent="dragging = true" @dragleave.prevent="dragging = false" @drop.prevent="drop">
            <div class="upload-card__drop">
                <template v-if="entry">
                    <video v-if="kind === 'video'" :src="entry.url" class="media-input__preview" controls playsinline preload="metadata" />
                    <audio v-else :src="entry.url" class="media-input__preview" controls preload="metadata" />
                    <div class="media-input__name">{{ entry.file.name }}</div>
                    <div v-if="entry.duration" class="hint">{{ entry.duration.toFixed(2) }}s{{ entry.frames.length ? ` · ${entry.frames.length} frames prepared` : '' }}</div>
                </template>
                <template v-else>
                    <div>Drop {{ kind === "video" ? "a video file" : "an audio file" }} here or select one.</div>
                    <div class="hint">Up to {{ kind === "video" ? MAX_VIDEO_SECONDS : MAX_AUDIO_SECONDS }} seconds and 256 MiB.</div>
                </template>
            </div>
            <div class="upload-card__actions">
                <button class="btn-ghost" type="button" @click="input?.click()">{{ entry ? "Replace" : "Select" }}</button>
                <button v-if="entry" class="btn-ghost" type="button" @click="$emit('clear')">{{ entry.processing ? "Cancel" : "Clear" }}</button>
                <input ref="input" type="file" :aria-label="kind === 'video' ? 'Reference video file' : 'Reference audio file'" @change="pick" />
            </div>
        </div>
        <label v-if="allowSoundtrack && entry?.frames.length" class="checkbox">
            <input type="checkbox" :checked="entry.useAudio" @change="$emit('soundtrack', ($event.target as HTMLInputElement).checked)" />
            <span>Use video soundtrack</span>
        </label>
        <div v-if="entry?.processing" class="hint" role="status">{{ entry.progress }}</div>
        <div v-if="entry?.error || (allowSoundtrack && entry?.useAudio && entry?.audioError)" class="status-message status-message--error" role="alert">{{ entry.error || entry.audioError }}</div>
        <div v-if="kind === 'video'" class="hint">Video is prepared in your browser. Use a short reference clip; larger frames are reduced to fit a 1280 px edge. The model may use only part of the clip.</div>
        <div v-else class="hint">Audio is prepared in your browser. MiniMax-H3 references cannot be combined with start/end frames; Wan S2V can use a start image with driving audio.</div>
    </div>
</template>
