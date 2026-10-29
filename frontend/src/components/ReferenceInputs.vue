<script setup lang="ts">
import { computed, ref } from "vue";
import ChevronIcon from "./ChevronIcon.vue";
import { createStoredRef } from "../lib/settings";
import type { GenerationFeatures, GenerationForm, GenerationMode, ImageEntry, ImageTarget, MediaReference } from "../lib/types";

type InputTarget = ImageTarget | "ref_videos" | "ref_audios";
interface InputItem { target: InputTarget; label: string; kind: "image" | "video" | "audio"; multiple?: boolean; }

const props = defineProps<{ form: GenerationForm; mode: GenerationMode; features: GenerationFeatures }>();
const emit = defineEmits<{
    (event: "selectImage", target: ImageTarget, files: FileList): void;
    (event: "clearImage", target: ImageTarget): void;
    (event: "removeImage", target: "ref_images" | "control_frames", index: number): void;
    (event: "replaceImage", target: "ref_images" | "control_frames", image: ImageEntry, files: FileList): void;
    (event: "preview", image: ImageEntry): void;
    (event: "selectMedia", kind: "video" | "audio", files: FileList): void;
    (event: "clearMedia", kind: "video" | "audio"): void;
    (event: "previewMedia", trigger: MouseEvent): void;
}>();
const dragging = ref<InputTarget | null>(null);
const expanded = createStoredRef<boolean>("sdcpp-webui-reference-inputs-expanded", true, (value: unknown) => value !== false);
const fileInputs: Partial<Record<InputTarget, HTMLInputElement>> = {};
const groups = computed(() => {
    const rows: { label: string; inputs: InputItem[] }[] = [
        { label: "Initial images and masks", inputs: [
            { target: "init_image", label: props.mode === "video" ? "Start Frame" : "Init Image", kind: "image" },
            { target: "end_image", label: "End Frame", kind: "image" },
            { target: "mask_image", label: "Mask Image", kind: "image" },
        ] },
        { label: "Control inputs", inputs: [
            { target: "control_image", label: "Control Image", kind: "image" },
            { target: "ip_adapter_image", label: "IP-Adapter Image", kind: "image" },
            { target: "control_frames", label: "Control Frames", kind: "image", multiple: true },
        ] },
        { label: "Reference media", inputs: [
            { target: "ref_videos", label: "Reference Video", kind: "video" },
            { target: "ref_audios", label: "Reference Audio", kind: "audio" },
            { target: "ref_images", label: "Reference Images", kind: "image", multiple: true },
        ] },
    ];
    return rows.map((row) => ({ ...row, inputs: row.inputs.filter((input) => props.features[input.target]) }))
        .filter((row) => row.inputs.length);
});
const summary = computed(() => groups.value.flatMap((group) => group.inputs).flatMap((input) => {
    const count = images(input).length;
    const entry = media(input);
    if (!count && !entry) return [];
    const status = entry?.error || entry?.audioError ? " (error)" : entry?.processing ? " (preparing…)" : "";
    return [`${input.label}${count > 1 ? ` × ${count}` : ""}${status}`];
}).join(" · ") || "No files");

function images(input: InputItem): ImageEntry[] {
    if (input.target === "ref_videos" || input.target === "ref_audios") return [];
    const value = props.form[input.target];
    return Array.isArray(value) ? value : value ? [value] : [];
}

function media(input: InputItem): MediaReference | null {
    return input.target === "ref_videos" ? props.form.ref_video : input.target === "ref_audios" ? props.form.ref_audio : null;
}

function thumbnail(input: InputItem): string | undefined {
    return input.multiple ? undefined : images(input)[0]?.dataUrl || media(input)?.frames[0];
}

function hasSelection(input: InputItem): boolean {
    return images(input).length > 0 || Boolean(media(input));
}

function choose(input: InputItem, event: MouseEvent): void {
    if (!input.multiple && images(input)[0]) emit("preview", images(input)[0]);
    else if (media(input)) emit("previewMedia", event);
    else fileInputs[input.target]?.click();
}

function select(input: InputItem, files: FileList | null | undefined): void {
    if (!files?.length) return;
    if (input.target === "ref_videos") emit("selectMedia", "video", files);
    else if (input.target === "ref_audios") emit("selectMedia", "audio", files);
    else emit("selectImage", input.target, files);
}

function pick(input: InputItem, event: Event): void {
    const element = event.target as HTMLInputElement;
    select(input, element.files);
    element.value = "";
}

function requestReplacement(event: MouseEvent): void {
    (event.currentTarget as HTMLElement).parentElement?.querySelector<HTMLInputElement>('input[type="file"]')?.click();
}

function pickReplacement(input: InputItem, image: ImageEntry, event: Event): void {
    const element = event.target as HTMLInputElement;
    if (element.files?.length && (input.target === "ref_images" || input.target === "control_frames")) {
        emit("replaceImage", input.target, image, element.files);
    }
    element.value = "";
}

function drop(input: InputItem, event: DragEvent): void {
    dragging.value = null;
    select(input, event.dataTransfer?.files);
}

function clear(input: InputItem): void {
    if (input.target === "ref_videos") emit("clearMedia", "video");
    else if (input.target === "ref_audios") emit("clearMedia", "audio");
    else emit("clearImage", input.target);
}

function remove(input: InputItem, index: number): void {
    if (input.target === "ref_images" || input.target === "control_frames") emit("removeImage", input.target, index);
}
</script>

<template>
    <div v-if="groups.length" class="reference-inputs" aria-label="Media inputs">
        <button class="reference-inputs__toggle" type="button" :aria-expanded="expanded" aria-controls="reference-inputs-rows" @click="expanded = !expanded">
            <ChevronIcon class="reference-inputs__chevron" />
            <span class="reference-inputs__title">Media inputs</span>
            <span v-if="!expanded" class="reference-inputs__summary" :title="summary">· {{ summary }}</span>
        </button>
        <div v-show="expanded" id="reference-inputs-rows" class="reference-inputs__rows">
            <div v-for="group in groups" :key="group.label" class="reference-inputs__row" role="group" :aria-label="group.label">
                <div v-for="input in group.inputs" :key="input.target" class="reference-input" :class="{ 'reference-input--dragging': dragging === input.target }"
                    @dragover.prevent.stop="dragging = input.target" @dragleave.self="dragging = null" @drop.prevent.stop="drop(input, $event)">
                    <template v-if="input.multiple">
                        <div v-for="(image, index) in images(input)" :key="`${index}-${image.name}`" class="reference-input__item">
                            <button class="reference-input__tile" type="button" :aria-label="`Preview ${input.label} ${index + 1}`" :title="image.name" @click="emit('preview', image)"><img :src="image.dataUrl" :alt="image.name" /></button>
                            <span class="reference-input__label">{{ index + 1 }}</span>
                            <button class="reference-input__remove" type="button" :aria-label="`Remove ${input.label} ${index + 1}`" @click="remove(input, index)">×</button>
                            <button class="reference-input__replace" type="button" :aria-label="`Replace ${input.label} ${index + 1}`" :title="`Replace ${input.label} ${index + 1}`" @click="requestReplacement">
                                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 7v5h-5M20 12a8 8 0 1 0-2.34 5.66" /></svg>
                            </button>
                            <input type="file" accept="image/*" :aria-label="`Replace ${input.label} ${index + 1}`" hidden @change="pickReplacement(input, image, $event)" />
                        </div>
                    </template>
                    <div class="reference-input__item">
                        <button class="reference-input__tile" :class="{ 'reference-input__tile--selected': hasSelection(input) }" type="button"
                            :aria-label="`${hasSelection(input) && !input.multiple ? 'View' : 'Add'} ${input.label}`"
                            :title="input.multiple ? `Add ${input.label}` : media(input)?.file.name || images(input)[0]?.name || `Add ${input.label}`" @click="choose(input, $event)">
                            <img v-if="thumbnail(input)" :src="thumbnail(input)" alt="" />
                            <svg v-else width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
                                <path v-if="input.multiple || !hasSelection(input)" d="M12 5v14M5 12h14" />
                                <template v-else-if="input.kind === 'video'"><rect x="3" y="6" width="12" height="12" rx="2"/><path d="m15 10 6-3v10l-6-3"/></template>
                                <path v-else d="M4 10v4m4-7v10m4-13v16m4-13v10m4-7v4"/>
                            </svg>
                        </button>
                        <span class="reference-input__label">{{ input.label }}</span>
                        <template v-if="!input.multiple && hasSelection(input)">
                            <button class="reference-input__remove" type="button" :aria-label="`Remove ${input.label}`" :title="`Remove ${input.label}`" @click="clear(input)">×</button>
                            <button class="reference-input__replace" type="button" :aria-label="`Replace ${input.label}`" :title="`Replace ${input.label}`" @click="fileInputs[input.target]?.click()">
                                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 7v5h-5M20 12a8 8 0 1 0-2.34 5.66" /></svg>
                            </button>
                        </template>
                        <span v-if="media(input)?.error || media(input)?.audioError" class="reference-input__status reference-input__status--error" role="alert" :title="media(input)?.error || media(input)?.audioError">Unable to prepare</span>
                        <span v-else-if="media(input)?.processing" class="reference-input__status" role="status" :title="media(input)?.progress">Preparing…</span>
                    </div>
                    <input :ref="(element) => { if (element) fileInputs[input.target] = element as HTMLInputElement; else delete fileInputs[input.target]; }" type="file" :accept="`${input.kind}/*`" :multiple="input.multiple" :aria-label="`Upload ${input.label}`" hidden @change="pick(input, $event)" />
                </div>
            </div>
        </div>
    </div>
</template>
