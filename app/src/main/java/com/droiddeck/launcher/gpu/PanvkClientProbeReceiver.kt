package com.droiddeck.launcher.gpu

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Environment
import android.util.Log
import com.droiddeck.launcher.core.HostProcess
import com.droiddeck.launcher.runtime.LinuxRuntime
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.session.SessionTerminal
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class PanvkClientProbeReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return
        val request = intent.getStringExtra("request_id")
        if (request == null || !request.matches(Regex("[a-f0-9]{32}"))) {
            resultCode = 2
            resultData = "Invalid PanVK proof request ID"
            return
        }
        if (!running.compareAndSet(false, true)) {
            resultCode = 2
            resultData = "PanVK client proof already running"
            return
        }
        try {
            val gpu = checkNotNull(VulkanInfo.cachedOrNull()) { "Vulkan GPU information is unavailable" }
            check(MaliSessionSelection.supported(gpu)) { "PanVK proof requires the validated Mali-G52 route" }
            val script = File(Environment.getExternalStorageDirectory(),
                "Download/DroidDeckPanVKTest/run_panvk_phone_test.py")
            check(script.isFile) { "Transfer the raw artifact and helpers first" }
            val builder = SessionTerminal.panvkProbeBuilder(script.path)
            val session = SessionState.sessionId
            val report = File(LinuxRuntime.logDir(context), "panvk-client-probe-$request.log")
            val exported = File(checkNotNull(context.getExternalFilesDir("panvk-client-probe")), report.name)
            report.parentFile?.mkdirs()
            report.writeText("DroidDeck development-only CLIENT PanVK proof; request=$request\n")
            val app = context.applicationContext
            // The live session owns the foreground service; GPU work outlives this broadcast.
            Thread({
                var process: Process? = null
                var pid = -1
                var exit = 1
                try {
                    check(SessionState.running && SessionState.sessionId == session) { "Session ended before PanVK proof startup" }
                    process = builder.redirectOutput(ProcessBuilder.Redirect.appendTo(report)).start()
                    pid = HostProcess.pidOf(process)
                    check(pid > 1) { "Cannot track the probe's runtime process" }
                    check(SessionState.running && SessionState.sessionId == session) { "Session ended during PanVK proof startup" }
                    SessionService.registerTerminalProcess(app, pid)
                    check(process.waitFor(70, TimeUnit.SECONDS)) { "PanVK client proof timed out" }
                    exit = process.exitValue()
                } catch (error: Exception) {
                    Log.e("PanvkClientProbe", "Client proof failed", error)
                    runCatching { report.appendText("\nC8B.1 PanVK CLIENT-ONLY PROBE FAIL: $error\n") }
                } finally {
                    try {
                        if (process?.isAlive == true) {
                            try {
                                if (pid > 1) SessionService.stopTerminalProcess(app, pid)
                            } finally {
                                process.destroy()
                            }
                        } else if (pid > 1) {
                            SessionService.terminalProcessExited(app, pid)
                        }
                    } catch (error: Exception) {
                        exit = 1
                        Log.e("PanvkClientProbe", "Client proof cleanup failed", error)
                    }
                    try {
                        report.appendText("\nDROIDDECK_PANVK_PROBE_DONE exit=$exit\n")
                        val staged = File(exported.parentFile, exported.name + ".staged")
                        report.copyTo(staged, overwrite = true)
                        check(staged.renameTo(exported)) { "Cannot export PanVK proof report" }
                    } catch (error: Exception) {
                        Log.e("PanvkClientProbe", "Client proof report export failed", error)
                    } finally {
                        running.set(false)
                    }
                }
            }, "panvk-client-proof").start()
            resultCode = 1
            resultData = exported.path
        } catch (error: Exception) {
            running.set(false)
            resultCode = 2
            resultData = "PanVK client proof unavailable: $error"
        }
    }

    companion object {
        const val ACTION = "com.droiddeck.launcher.PANVK_CLIENT_PROBE"
        private val running = AtomicBoolean(false)
    }
}
