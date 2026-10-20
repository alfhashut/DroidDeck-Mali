package com.droiddeck.launcher.gpu

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MaliSessionControlsTest {
    @Test fun onlyBoundedControlsEnterTheMaliEnvironment() {
        val result = MaliSessionControls.environment(listOf(
            "MALI_VULKAN_PERF_HZ=30", "MALI_VULKAN_PERF_HZ=60",
            "MALI_VULKAN_PERF_BASELINE=0", "MALI_VULKAN_VERBOSE=1",
            "MALI_VULKAN_SHARED_STAGING=1", "VK_DRIVER_FILES=/other/icd.json",
            "DROIDDECK_FRAME_TRACE_DIR=/storage/emulated/0/Download",
        ))
        assertEquals(listOf("MALI_VULKAN_PERF_HZ=60", "MALI_VULKAN_PERF_BASELINE=0", "MALI_VULKAN_VERBOSE=1"), result)
    }

    @Test fun malformedAndUnboundedValuesAreRejected() {
        assertTrue(MaliSessionControls.environment(listOf(
            "MALI_VULKAN_PERF_HZ=120", "MALI_VULKAN_PERF_HZ= 60",
            "MALI_VULKAN_PERF_BASELINE=true", "MALI_VULKAN_VERBOSE=2", "MALI_VULKAN_PERF_HZ",
        )).isEmpty())
    }
}
