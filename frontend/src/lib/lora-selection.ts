import { type Ref, computed, ref, watch } from "vue";
import { createStoredRef } from "./settings";
import type { FormLoraEntry } from "./types";

export function useLoraSelection(baseUrl: Ref<string>, recipeId: Ref<string | undefined>): Ref<FormLoraEntry[]> {
    const saved = createStoredRef<Record<string, FormLoraEntry[]>>("sdcpp-webui-lora-selections", {}, (value: unknown) => {
        if (value && typeof value === "object" && !Array.isArray(value) && Object.values(value).every((entries) =>
            Array.isArray(entries) && entries.every((entry) => entry && typeof entry.path === "string" &&
                typeof entry.multiplier === "number" && Number.isFinite(entry.multiplier) && typeof entry.is_high_noise === "boolean"))) {
            return value as Record<string, FormLoraEntry[]>;
        }
        return {};
    });
    const scope = computed(() => recipeId.value ? JSON.stringify([
        (baseUrl.value.trim() || window.location.origin + window.location.pathname).replace(/\/+$/, ""),
        recipeId.value,
    ]) : "");
    const entries = ref<FormLoraEntry[]>([]);
    let currentScope = "";

    watch(scope, (key) => {
        currentScope = "";
        entries.value = (saved.value[key] || []).map((entry) => ({ ...entry }));
        currentScope = key;
    }, { immediate: true, flush: "sync" });

    watch(entries, (value) => {
        if (!currentScope) return;
        saved.value = {
            ...saved.value,
            [currentScope]: value.map((entry) => ({
                ...entry,
                multiplier: typeof entry.multiplier === "number" && Number.isFinite(entry.multiplier) ? entry.multiplier : 1,
            })),
        };
    }, { deep: true, flush: "sync" });

    return entries;
}
