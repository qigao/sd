import { computed, onBeforeUnmount, ref, watch } from "vue";

import { createStoredRef } from "./settings";

type Theme = "system" | "light" | "dark";

export function useTheme() {
    const theme = createStoredRef<Theme>("sdcpp-webui-theme", "system", (value: unknown) =>
        value === "light" || value === "dark" ? value : "system");
    const systemTheme = window.matchMedia("(prefers-color-scheme: dark)");
    const systemDark = ref(systemTheme.matches);
    const resolvedTheme = computed(() => theme.value === "system"
        ? (systemDark.value ? "dark" : "light")
        : theme.value);
    const applyTheme = () => {
        document.documentElement.dataset.theme = resolvedTheme.value;
    };
    const updateSystemTheme = () => { systemDark.value = systemTheme.matches; };
    const toggleTheme = () => { theme.value = resolvedTheme.value === "dark" ? "light" : "dark"; };

    watch(resolvedTheme, applyTheme, { immediate: true });
    systemTheme.addEventListener("change", updateSystemTheme);
    onBeforeUnmount(() => systemTheme.removeEventListener("change", updateSystemTheme));

    return { theme, resolvedTheme, toggleTheme };
}
