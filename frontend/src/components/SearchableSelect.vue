<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from "vue";
import type { CSSProperties } from "vue";

const props = defineProps<{
    id: string;
    modelValue: string;
    options: { value: string; label: string }[];
    placeholder: string;
    disabled?: boolean;
    allowEmpty?: boolean;
    ariaLabel?: string;
    contained?: boolean;
}>();
const emit = defineEmits<{ (event: "update:modelValue", value: string): void }>();
const open = ref(false);
const query = ref("");
const active = ref(0);
const list = ref<HTMLElement | null>(null);
const input = ref<HTMLInputElement | null>(null);
const listPosition = ref<CSSProperties>({});
let observer: ResizeObserver | null = null;
const selectedLabel = computed(() => props.options.find((item) => item.value === props.modelValue)?.label || "");
const filtered = computed(() => {
    const options = props.allowEmpty ? [{ value: "", label: props.placeholder }, ...props.options] : props.options;
    const search = query.value.trim().toLocaleLowerCase();
    return options.filter((item) => `${item.label} ${item.value}`.toLocaleLowerCase().includes(search));
});

function positionList(): void {
    if (!open.value || !props.contained || !input.value) return;
    const rect = input.value.getBoundingClientRect();
    const bounds = input.value.closest<HTMLElement>(".composer-panel__body")?.getBoundingClientRect();
    const margin = 8;
    const gap = 4;
    const below = Math.min(bounds?.bottom ?? window.innerHeight, window.innerHeight) - rect.bottom - gap - margin;
    const above = rect.top - Math.max(bounds?.top ?? 0, 0) - gap - margin;
    const placeAbove = below < 260 && above > below;
    listPosition.value = {
        top: placeAbove ? "auto" : `${rect.height + gap}px`,
        bottom: placeAbove ? `${rect.height + gap}px` : "auto",
        maxHeight: `${Math.max(0, Math.min(260, placeAbove ? above : below))}px`,
    };
}

function stopPositioning(): void {
    window.removeEventListener("resize", positionList);
    window.removeEventListener("scroll", positionList, true);
    observer?.disconnect();
    observer = null;
}

watch([open, () => props.contained], ([shown, contained]) => {
    stopPositioning();
    if (!shown || !contained) { listPosition.value = {}; return; }
    positionList();
    window.addEventListener("resize", positionList);
    window.addEventListener("scroll", positionList, true);
    if (input.value) {
        observer = new ResizeObserver(positionList);
        observer.observe(input.value);
        const body = input.value.closest<HTMLElement>(".composer-panel__body");
        if (body) observer.observe(body);
    }
}, { flush: "post" });
onBeforeUnmount(stopPositioning);

function show(): void {
    if (props.disabled || open.value) return;
    query.value = "";
    active.value = Math.max(0, filtered.value.findIndex((item) => item.value === props.modelValue));
    open.value = true;
    void nextTick(() => {
        const option = list.value?.children[active.value] as HTMLElement | undefined;
        if (list.value && option) {
            list.value.scrollTop = option.offsetTop - (list.value.clientHeight - option.offsetHeight) / 2;
        }
    });
}

function choose(value: string): void {
    emit("update:modelValue", value);
    open.value = false;
}

function search(event: Event): void {
    query.value = (event.target as HTMLInputElement).value;
    active.value = 0;
    open.value = true;
    void nextTick(() => {
        if (list.value) list.value.scrollTop = 0;
    });
}

function onKeydown(event: KeyboardEvent): void {
    if (event.key === "Escape") {
        if (open.value) event.preventDefault();
        open.value = false;
    } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        if (!open.value) show();
        else active.value = Math.max(0, Math.min(filtered.value.length - 1, active.value + (event.key === "ArrowDown" ? 1 : -1)));
        void nextTick(() => list.value?.children[active.value]?.scrollIntoView({ block: "nearest" }));
    } else if (event.key === "Enter" && open.value) {
        event.preventDefault();
        const option = filtered.value[active.value];
        if (option) choose(option.value);
    } else if (event.key === "Tab") {
        open.value = false;
    }
}

watch(() => props.disabled, (disabled) => { if (disabled) open.value = false; });
watch(() => props.modelValue, () => { open.value = false; });
watch(filtered, (options, previous) => {
    const value = previous[active.value]?.value;
    active.value = Math.max(0, options.findIndex((item) => item.value === value));
}, { flush: "sync" });
</script>

<template>
    <div class="searchable-select">
        <input :id="id" ref="input" type="text" role="combobox" autocomplete="off" :disabled="disabled"
            :value="open ? query : selectedLabel" :placeholder="open ? 'Type to search…' : placeholder"
            :title="selectedLabel" :aria-label="ariaLabel" :aria-expanded="open" aria-autocomplete="list" aria-haspopup="listbox"
            :aria-controls="id + '-options'" :aria-activedescendant="open && filtered.length ? id + '-option-' + active : undefined"
            @focus="show" @click="show" @input="search" @keydown="onKeydown" @blur="open = false" />
        <span class="searchable-select__arrow" aria-hidden="true">▾</span>
        <ul v-if="open" :id="id + '-options'" ref="list" class="searchable-select__options" :style="listPosition" role="listbox"
            :aria-label="ariaLabel" :aria-labelledby="ariaLabel ? undefined : id + '-label'" @mousedown.prevent>
            <li v-for="(item, index) in filtered" :id="id + '-option-' + index" :key="item.value" role="option"
                :aria-selected="item.value === modelValue" :class="{ 'is-active': index === active }"
                @mouseenter="active = index" @click="choose(item.value)">{{ item.label }}</li>
            <li v-if="!filtered.length" class="searchable-select__empty" role="presentation">No matches</li>
        </ul>
    </div>
</template>

<style scoped>
.searchable-select { position: relative; min-width: 0; height: 42px; }
.searchable-select:focus-within { z-index: 20; }
.searchable-select input { width: 100%; height: 42px; border: 1px solid var(--border); border-radius: var(--radius-sm); padding: 10px 32px 10px 12px; background: var(--input-bg); color: var(--text); outline: none; text-overflow: ellipsis; }
.searchable-select input:focus { border-color: var(--focus-border); box-shadow: 0 0 0 3px var(--focus-ring); }
.searchable-select input:disabled { cursor: not-allowed; opacity: 0.6; }
.searchable-select__arrow { position: absolute; right: 12px; top: 50%; transform: translateY(-50%); pointer-events: none; color: var(--muted); }
.searchable-select__options { position: absolute; top: calc(100% + 4px); left: 0; right: 0; z-index: 20; max-height: 260px; overflow-y: auto; overscroll-behavior: contain; margin: 0; padding: 4px; list-style: none; background: var(--panel); border: 1px solid var(--border); border-radius: var(--radius-sm); box-shadow: var(--shadow); }
.searchable-select__options li { padding: 8px; border-radius: 4px; overflow-wrap: anywhere; cursor: pointer; }
.searchable-select__options li.is-active, .searchable-select__options li[role="option"]:hover { background: var(--primary-soft); }
.searchable-select__options li[aria-selected="true"], .searchable-select__options li[aria-selected="true"]:hover { background: var(--select-option-selected-bg); color: var(--primary); font-weight: 600; }
.searchable-select__options .searchable-select__empty { color: var(--muted); cursor: default; }
</style>
