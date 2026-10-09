package com.droiddeck.launcher.gpu

/** Scope Auto to the phone-validated device. Names are informational, never the gate. */
object MaliSessionSelection {
    const val MODE = "mali-wayland"
    fun supported(info: Map<String, String>): Boolean {
        fun id(key: String) = info[key]?.removePrefix("0x")?.removePrefix("0X")?.toLongOrNull(16)
        val api = info["api"]?.split('.')?.mapNotNull { it.toIntOrNull() } ?: return false
        return info["error"] == null && id("vendor_id") == 0x13b5L && id("device_id") == 0x74021000L &&
            api.size == 3 && (api[0] > 1 || (api[0] == 1 && api[1] >= 1))
    }
    fun environment(manifest: String, socket: String): List<String> = listOf(
        "VK_DRIVER_FILES=$manifest", "VK_ICD_FILENAMES=$manifest",
        "MALI_VULKAN_BROKER_SOCKET=$socket", "MALI_VULKAN_NORMAL_SESSION=1",
        "MALI_VULKAN_RENDERER_TEST=1", "MALI_VULKAN_AHB_TEST=1", "VK_LOADER_LAYERS_DISABLE=*",
    )
}
