package com.droiddeck.launcher.gpu

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Environment
import android.util.Log
import com.droiddeck.launcher.core.HostProcess
import com.droiddeck.launcher.runtime.LinuxRuntime
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionTerminal
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit shell/DUMP-only development trigger; normal Launch never calls this receiver. */
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
            val script = File(Environment.getExternalStorageDirectory(),
                "Download/DroidDeckPanVKTest/run_panvk_phone_test.py")
            check(script.isFile) { "Transfer the raw artifact and helpers first" }
            val builder = SessionTerminal.panvkProbeBuilder(script.path)
            val report = File(LinuxRuntime.debugLogDir(), "panvk-client-probe-$request.log")
            report.parentFile?.mkdirs()
            report.writeText("DroidDeck development-only CLIENT PanVK proof; request=$request\n")
            val app = context.applicationContext
            // The required live session already owns a foreground service. Do not hold
            // a broadcast open across GPU work or switch away from its compositor UI.
            Thread({
                var process: Process? = null
                var pid = -1
                var exit = 1
                try {
                    process = builder.redirectOutput(ProcessBuilder.Redirect.appendTo(report)).start()
                    pid = HostProcess.pidOf(process)
                    check(pid > 1) { "Cannot track the probe's runtime process" }
                    SessionService.registerTerminalProcess(app, pid)
                    check(process.waitFor(70, TimeUnit.SECONDS)) { "PanVK client proof timed out" }
                    exit = process.exitValue()
                } catch (error: Exception) {
                    Log.e("PanvkClientProbe", "Client proof failed", error)
                    report.appendText("\nC8B.1 PanVK CLIENT-ONLY PROBE FAIL: $error\n")
                } finally {
                    if (process?.isAlive == true) {
                        if (pid > 1) SessionService.stopTerminalProcess(app, pid)
                        process.destroy()
                    } else if (pid > 1) {
                        SessionService.terminalProcessExited(app, pid)
                    }
                    try {
                        report.appendText("\nDROIDDECK_PANVK_PROBE_DONE exit=$exit\n")
                    } finally {
                        running.set(false)
                    }
                }
            }, "panvk-client-proof").start()
            resultCode = 1
            resultData = report.path
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
