<script setup lang="ts">
import { computed, nextTick, ref, shallowRef, watch } from "vue";
import ComposerPanel from "./ComposerPanel.vue";
import ChevronIcon from "./ChevronIcon.vue";
import ResolutionPicker from "./ResolutionPicker.vue";
import { aspectLabel, dimensionError } from "../lib/resolution";
import { createStoredRef } from "../lib/settings";
import type { Dimensions } from "../lib/resolution";
import type { Capabilities, GenerationForm, GenerationMode } from "../lib/types";

const props = defineProps<{
    form: GenerationForm;
    mode: GenerationMode;
    modelLabel: string;
    modelReady: boolean;
    disabled: boolean;
    busy: boolean;
    canCancel: boolean;
    cancelling: boolean;
    hasLoras: boolean;
    loraCount: number;
    limits?: Capabilities["limits"];
    sizeMultiple: number;
    recommended: Dimensions;
    videoSummary: string;
    videoDetails: string;
}>();
const emit = defineEmits<{
    (event: "generate"): void;
    (event: "cancel"): void;
}>();
const activePanel = ref("");
const panelAnchor = shallowRef<HTMLElement | null>(null);
const textarea = ref<HTMLTextAreaElement | null>(null);
const negativeTextarea = ref<HTMLTextAreaElement | null>(null);
const negativeExpanded = createStoredRef<boolean>("sdcpp-webui-negative-prompt-expanded", false, (value: unknown) => value === true);
const negativeSummary = computed(() => props.form.negative_prompt.replace(/\s+/g, " ").trim());
const showNegativePrompt = computed(() => props.form.sample_params.guidance.txt_cfg !== 1);
const titles: Record<string, string> = { models: "Model", resolution: "Aspect Ratio & Resolution", output: "Output", lora: "LoRA", sample: "Sample & Guidance", advanced: "More settings" };
const size = computed(() => ({ width: props.form.width, height: props.form.height }));
const sizeError = computed(() => dimensionError(size.value, props.limits, props.sizeMultiple));
const buttonLabel = computed(() => props.busy ? "Generating…" : "Generate");
const seedLabel = computed(() => props.form.seed < 0 ? "Random" : String(props.form.seed));
const seedSummary = computed(() => seedLabel.value.length > 10
    ? `${seedLabel.value.slice(0, 6)}…${seedLabel.value.slice(-4)}`
    : seedLabel.value);
const samplingSummary = computed(() => {
    const sample = props.form.sample_params;
    return `${sample.scheduler || 'default'} · ${sample.sample_method || 'default'} · CFG ${sample.guidance.txt_cfg} · ${sample.sample_steps} steps`;
});
const samplingTitle = computed(() => {
    const sample = props.form.sample_params;
    const flowShift = sample.flow_shift === "" || sample.flow_shift == null ? "auto" : sample.flow_shift;
    return `Scheduler: ${sample.scheduler || 'default'}; Method: ${sample.sample_method || 'default'}; CFG: ${sample.guidance.txt_cfg}; Steps: ${sample.sample_steps}; Flow Shift: ${flowShift}`;
});

function open(panel: string, event?: MouseEvent, toggle = true): void {
    panelAnchor.value = event?.currentTarget instanceof HTMLElement ? event.currentTarget : null;
    activePanel.value = toggle && activePanel.value === panel ? "" : panel;
}

function setSize(next: Dimensions): void {
    props.form.width = next.width;
    props.form.height = next.height;
}

function submit(event?: KeyboardEvent): void {
    if (event?.isComposing) return;
    if (!props.disabled && !props.busy && !sizeError.value) emit("generate");
}

async function resizePrompt(): Promise<void> {
    await nextTick();
    if (!textarea.value) return;
    textarea.value.style.height = "auto";
    textarea.value.style.height = `${Math.min(180, Math.max(76, textarea.value.scrollHeight))}px`;
}

async function resizeNegativePrompt(): Promise<void> {
    await nextTick();
    if (!showNegativePrompt.value || !negativeExpanded.value || !negativeTextarea.value) return;
    negativeTextarea.value.style.height = "auto";
    negativeTextarea.value.style.height = `${Math.min(96, Math.max(52, negativeTextarea.value.scrollHeight))}px`;
}

async function toggleNegativePrompt(): Promise<void> {
    negativeExpanded.value = !negativeExpanded.value;
    if (negativeExpanded.value) {
        await nextTick();
        negativeTextarea.value?.focus();
    }
}

function clearNegativePrompt(): void {
    props.form.negative_prompt = "";
    negativeTextarea.value?.focus();
}

watch(() => props.form.prompt, resizePrompt, { immediate: true });
watch([() => props.form.negative_prompt, negativeExpanded, showNegativePrompt], resizeNegativePrompt, { immediate: true });
watch(() => props.mode, () => {
    if (activePanel.value !== "models") activePanel.value = "";
    resizePrompt();
});
watch(() => props.hasLoras, (available) => { if (!available && activePanel.value === "lora") activePanel.value = ""; });
defineExpose({
    close: () => { activePanel.value = ""; },
    openModels: (event?: MouseEvent) => open("models", event, false),
    openMore: (event?: MouseEvent) => open("advanced", event, false),
});
</script>

<template>
    <section class="composer" aria-label="Generation prompt and settings">
        <ComposerPanel :anchor="panelAnchor" :centered="activePanel === 'models' || activePanel === 'lora'" :min-height="activePanel === 'lora' ? 420 : undefined" :open="Boolean(activePanel)" :title="titles[activePanel] || 'Settings'" @close="activePanel = ''">
            <div v-show="activePanel === 'models'"><slot name="models" /></div>
            <div v-show="activePanel === 'resolution'">
                <ResolutionPicker :width="form.width" :height="form.height" :mode="mode" :limits="limits" :size-multiple="sizeMultiple" :recommended="recommended" @change="setSize" />
            </div>
            <div v-show="activePanel === 'output'"><slot name="output" /></div>
            <div v-show="activePanel === 'lora'"><slot name="lora" /></div>
            <div v-show="activePanel === 'sample'"><slot name="sample" /></div>
            <div v-show="activePanel === 'advanced'"><slot name="advanced" /></div>
        </ComposerPanel>

        <slot name="inputs" />
        <label class="sr-only" for="generation-prompt">Prompt</label>
        <textarea id="generation-prompt" ref="textarea" v-model="form.prompt" class="composer-prompt" rows="3"
            :placeholder="mode === 'video' ? 'Describe a scene. Bring it to life…' : 'Describe the image you have in mind…'"
            @keydown.ctrl.enter.prevent="submit" @keydown.meta.enter.prevent="submit" />

        <div v-if="showNegativePrompt" class="composer-negative">
            <div class="composer-negative__header">
                <button class="composer-negative__toggle" type="button" :aria-expanded="negativeExpanded" aria-controls="negative-prompt-region" @click="toggleNegativePrompt">
                    <ChevronIcon class="composer-negative__chevron" />
                    <span id="negative-prompt-label" class="composer-negative__label">Negative prompt</span>
                    <span v-if="!negativeExpanded && negativeSummary" class="composer-negative__summary" :title="negativeSummary">· {{ negativeSummary }}</span>
                </button>
                <button v-if="negativeExpanded && form.negative_prompt" class="composer-negative__clear" type="button" aria-label="Clear negative prompt" @click="clearNegativePrompt">Clear</button>
            </div>
            <div v-show="negativeExpanded" id="negative-prompt-region">
                <textarea id="negative-prompt" ref="negativeTextarea" v-model="form.negative_prompt" class="composer-negative__input" rows="2" aria-labelledby="negative-prompt-label"
                    placeholder="What should be excluded?" @keydown.ctrl.enter.prevent="submit" @keydown.meta.enter.prevent="submit" />
            </div>
        </div>

        <div class="composer-toolbar">
            <div class="composer-toolbar__options">
                <button class="composer-chip composer-chip--model" type="button" :aria-expanded="activePanel === 'models'" aria-controls="composer-panel" @click="open('models', $event)"><span class="model-dot" :class="{ 'model-dot--ready': modelReady }" aria-hidden="true"></span><span>{{ modelLabel }}</span><ChevronIcon class="composer-chevron" /></button>
                <button v-if="hasLoras" class="composer-chip" type="button" :aria-expanded="activePanel === 'lora'" aria-controls="composer-panel" @click="open('lora', $event)">LoRA<span v-if="loraCount"> · {{ loraCount }}</span></button>
                <button class="composer-chip" type="button" aria-label="Aspect Ratio and Resolution" :aria-expanded="activePanel === 'resolution'" aria-controls="composer-panel" @click="open('resolution', $event)"><span class="toolbar-ratio" aria-hidden="true"></span>{{ aspectLabel(size, sizeMultiple) }} · {{ sizeError ? 'Custom size' : `${form.width} × ${form.height}` }} <ChevronIcon class="composer-chevron" /></button>
                <button class="composer-chip composer-chip--sample" type="button" :title="samplingTitle" :aria-label="`Sample & Guidance. ${samplingTitle}`" :aria-expanded="activePanel === 'sample'" aria-controls="composer-panel" @click="open('sample', $event)"><span class="composer-chip__summary">{{ samplingSummary }}</span><ChevronIcon class="composer-chevron" /></button>
                <button class="composer-chip" type="button" :aria-label="`${mode === 'image' ? 'Number of images and seed' : videoDetails}. Seed: ${seedLabel}`" :title="`${mode === 'video' ? `${videoDetails}; ` : ''}Seed: ${seedLabel}`" :aria-expanded="activePanel === 'output'" aria-controls="composer-panel" @click="open('output', $event)">{{ mode === 'image' ? `${form.batch_count} image${form.batch_count === 1 ? '' : 's'}` : videoSummary }} · {{ seedSummary }} <ChevronIcon class="composer-chevron" /></button>
                <button class="composer-chip" type="button" :aria-expanded="activePanel === 'advanced'" aria-controls="composer-panel" @click="open('advanced', $event)">More <span aria-hidden="true">···</span></button>
            </div>
            <button v-if="busy" class="btn-danger composer-submit" type="button" :disabled="!canCancel || cancelling" @click="emit('cancel')">{{ cancelling ? 'Cancelling…' : 'Cancel' }}</button>
            <button v-else class="btn composer-submit" type="button" :disabled="disabled || Boolean(sizeError)" @click="submit()">{{ buttonLabel }}</button>
        </div>
        <div class="composer-caption" aria-live="polite">
            <span v-if="sizeError" class="composer-error">{{ sizeError }}</span>
            <span v-else-if="busy">Generation in progress. You can prepare the next prompt.</span>
            <span v-else-if="modelReady">{{ mode === 'video' ? 'Video' : 'Image' }} generation</span>
            <span class="composer-shortcut">Ctrl / ⌘ + Enter</span>
        </div>
    </section>
</template>
