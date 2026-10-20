package com.droiddeck.launcher.gpu

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class MaliSharedStagingTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun missingStorageMarkerAndDirectoryMarkerDefaultToBulk() {
        assertEquals("MALI_VULKAN_SHARED_STAGING=0", MaliNormalBroker.sharedStagingEnvironment(null))
        val directory = temporary.newFolder("mali-vulkan")
        assertEquals("MALI_VULKAN_SHARED_STAGING=0", MaliNormalBroker.sharedStagingEnvironment(directory))
        File(directory, "shared-staging.enabled").mkdir()
        assertEquals("MALI_VULKAN_SHARED_STAGING=0", MaliNormalBroker.sharedStagingEnvironment(directory))
    }

    @Test fun explicitMarkerSelectsSharedAndRemovalRestoresBulkForNextStartup() {
        val directory = temporary.newFolder("mali-vulkan")
        val marker = File(directory, "shared-staging.enabled")
        marker.writeText("")
        assertEquals("MALI_VULKAN_SHARED_STAGING=1", MaliNormalBroker.sharedStagingEnvironment(directory))
        marker.delete()
        assertEquals("MALI_VULKAN_SHARED_STAGING=0", MaliNormalBroker.sharedStagingEnvironment(directory))
        marker.writeText("")
        assertEquals("MALI_VULKAN_SHARED_STAGING=1", MaliNormalBroker.sharedStagingEnvironment(directory))
    }
}
