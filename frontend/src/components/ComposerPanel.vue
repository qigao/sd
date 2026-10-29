<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { CSSProperties } from "vue";

const props = defineProps<{ open: boolean; title: string; anchor?: HTMLElement | null; centered?: boolean; minHeight?: number }>();
const emit = defineEmits<{ (event: "close"): void }>();
const panel = ref<HTMLElement | null>(null);
const closeButton = ref<HTMLButtonElement | null>(null);
const position = ref<CSSProperties>({});
let trigger: HTMLElement | null = null;
let observer: ResizeObserver | null = null;
let anchorRect: DOMRect | null = null;

function measure(): void {
    if (!props.open || !panel.value) return;
    if (props.centered) {
        position.value = {
            minHeight: props.minHeight ? `min(${props.minHeight}px, calc(100dvh - 48px))` : undefined,
        };
        return;
    }
    const anchor = props.anchor || panel.value.parentElement;
    const bounds = anchor?.isConnected ? anchor.getBoundingClientRect() : null;
    if (bounds && (bounds.width || bounds.height)) anchorRect = bounds;
    if (window.innerWidth <= 720) {
        position.value = props.minHeight ? { minHeight: `min(${props.minHeight}px, 82dvh)` } : {};
        return;
    }
    const rect = anchorRect;
    if (!rect) return;
    const margin = 16;
    const gap = 12;
    const width = Math.min(760, window.innerWidth - margin * 2);
    const left = Math.max(margin, Math.min(rect.left + rect.width / 2 - width / 2, window.innerWidth - width - margin));
    const above = rect.top - gap - margin;
    const below = window.innerHeight - rect.bottom - gap - margin;
    const placeAbove = above >= below;
    const available = placeAbove ? above : below;
    const maxHeight = Math.min(680, available < 260 ? window.innerHeight - margin * 2 : available);
    position.value = {
        left: `${left}px`, width: `${width}px`,
        top: available < 260 ? 'auto' : placeAbove ? 'auto' : `${rect.bottom + gap}px`,
        bottom: available < 260 ? `${margin}px` : placeAbove ? `${window.innerHeight - rect.top + gap}px` : 'auto',
        maxHeight: `${maxHeight}px`,
        minHeight: props.minHeight ? `${Math.min(props.minHeight, maxHeight)}px` : undefined,
    };
}

watch(() => props.open, async (open) => {
    if (open) {
        trigger = props.anchor || (document.activeElement instanceof HTMLElement ? document.activeElement : null);
        anchorRect = null;
        measure();
        await nextTick();
        measure();
        closeButton.value?.focus();
    } else {
        trigger?.focus();
    }
});
watch([() => props.anchor, () => props.centered, () => props.minHeight], async ([anchor]) => {
    if (!props.open) return;
    trigger = anchor || trigger;
    anchorRect = null;
    measure();
    await nextTick();
    measure();
});

onMounted(() => {
    window.addEventListener("resize", measure);
    window.addEventListener("scroll", measure, true);
    if (panel.value?.parentElement) {
        observer = new ResizeObserver(measure);
        observer.observe(panel.value.parentElement);
    }
});
onBeforeUnmount(() => {
    window.removeEventListener("resize", measure);
    window.removeEventListener("scroll", measure, true);
    observer?.disconnect();
});

function onKeydown(event: KeyboardEvent): void {
    if (event.defaultPrevented) return;
    if (event.key === "Escape") {
        event.stopPropagation();
        emit("close");
    } else if (event.key === "Tab") {
        const elements = [...(panel.value?.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), a[href], summary') || [])]
            .filter((element) => element.getClientRects().length > 0);
        const first = elements[0];
        const last = elements[elements.length - 1];
        if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last?.focus(); }
        else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first?.focus(); }
    }
}
</script>

<template>
    <div v-show="open" class="composer-backdrop" @click="emit('close')" @wheel.prevent @touchmove.prevent></div>
    <section v-show="open" id="composer-panel" ref="panel" class="composer-panel" :class="{ 'composer-panel--centered': centered }" :style="position" role="dialog" aria-modal="true" aria-labelledby="composer-panel-title" @keydown="onKeydown">
        <div class="composer-panel__header"><h2 id="composer-panel-title">{{ title }}</h2><button ref="closeButton" class="btn-ghost" type="button" aria-label="Close configuration" @click="emit('close')">Done</button></div>
        <div class="composer-panel__body"><slot /></div>
    </section>
</template>
