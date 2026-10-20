package com.droiddeck.launcher.gpu

object MaliSessionControls {
    private val allowed = mapOf(
        "MALI_VULKAN_PERF_BASELINE" to setOf("0", "1"),
        "MALI_VULKAN_PERF_HZ" to setOf("30", "60"),
        "MALI_VULKAN_VERBOSE" to setOf("0", "1"),
    )

    fun environment(entries: List<String>): List<String> {
        val selected = LinkedHashMap<String, String>()
        entries.forEach { entry ->
            val key = entry.substringBefore('=')
            val value = entry.substringAfter('=', "")
            if (allowed[key]?.contains(value) == true) selected[key] = value
        }
        return selected.map { (key, value) -> "$key=$value" }
    }
}
