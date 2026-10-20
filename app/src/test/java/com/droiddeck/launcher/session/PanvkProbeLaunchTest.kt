package com.droiddeck.launcher.session

import java.io.File
import org.junit.After
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class PanvkProbeLaunchTest {
    @After fun reset() {
        SessionTerminal.clear()
        SessionState.running = false
        SessionState.maliBackend = false
        SessionState.mode = SessionService.MODE_STEAM
    }

    private fun prepare() {
        SessionState.running = true
        SessionState.maliBackend = true
        SessionState.mode = SessionService.MODE_STEAM
        SessionTerminal.prepare(listOf("/runtime/proot", "-r", "/guest", "/usr/bin/env", "-i", "DISPLAY=:42",
            "/bin/bash", "-c", "cd \"\$HOME\" && exec /bin/bash -i"),
            arrayOf("PROOT_TMP_DIR=/private/tmp"), File("/guest"), 7)
    }

    @Test fun proofReusesThePreparedRuntimeAndReplacesOnlyTheInteractiveShell() {
        prepare()
        val command = SessionTerminal.panvkProbeBuilder("/transfer/run_panvk_phone_test.py")
        assertEquals(listOf("/runtime/proot", "-r", "/guest", "/usr/bin/env", "-i", "DISPLAY=:42",
            "/usr/bin/python3", "-B", "/transfer/run_panvk_phone_test.py"), command.command())
        assertEquals(mapOf("PROOT_TMP_DIR" to "/private/tmp"), command.environment())
        assertEquals(File("/guest"), command.directory())
        assertTrue(command.redirectErrorStream())
    }

    @Test fun clearedAndNonMaliSessionsCannotRunAProof() {
        prepare()
        SessionState.maliBackend = false
        assertThrows(IllegalStateException::class.java) { SessionTerminal.panvkProbeBuilder("/proof.py") }
        SessionState.maliBackend = true
        SessionState.running = false
        assertThrows(IllegalStateException::class.java) { SessionTerminal.panvkProbeBuilder("/proof.py") }
        SessionState.running = true
        SessionState.mode = SessionService.MODE_DESKTOP
        assertThrows(IllegalStateException::class.java) { SessionTerminal.panvkProbeBuilder("/proof.py") }
        SessionState.mode = SessionService.MODE_STEAM
        SessionTerminal.clear(7)
        assertThrows(IllegalStateException::class.java) { SessionTerminal.panvkProbeBuilder("/proof.py") }
    }
}
