package com.droiddeck.launcher.gpu

import org.junit.Assert.*
import org.junit.Test

class MaliSessionSelectionTest {
    private fun device(vendor: String = "0x13b5", device: String = "0x74021000", api: String = "1.1.131") =
        mapOf("vendor_id" to vendor, "device_id" to device, "api" to api, "name" to "arbitrary name")

    @Test fun validatedIdsSelectMaliWithoutNameMatching() {
        assertTrue(MaliSessionSelection.supported(device()))
        assertTrue(MaliSessionSelection.supported(device("13B5", "74021000")))
    }
    @Test fun adrenoAndOtherDevicesKeepTheirPath() {
        assertFalse(MaliSessionSelection.supported(device("0x5143", "0x07030001") + ("name" to "Mali-G52")))
        assertFalse(MaliSessionSelection.supported(device(device = "0x74022000")))
        assertFalse(MaliSessionSelection.supported(device(api = "1.0.131")))
        assertFalse(MaliSessionSelection.supported(device() + ("error" to "loader failed")))
        assertFalse(MaliSessionSelection.supported(emptyMap()))
    }
    @Test fun guestOverridesAreExplicitAndLocal() {
        val env = MaliSessionSelection.environment("/app/icd.json", "/app/broker.sock")
        assertTrue("VK_DRIVER_FILES=/app/icd.json" in env)
        assertTrue("VK_ICD_FILENAMES=/app/icd.json" in env)
        assertTrue("MALI_VULKAN_BROKER_SOCKET=/app/broker.sock" in env)
        assertTrue("MALI_VULKAN_NORMAL_SESSION=1" in env)
        assertFalse(env.any { it.startsWith("MALI_VULKAN_SESSION_TEST=") || it.startsWith("BL_VK_DRIVER=") })
    }
}
