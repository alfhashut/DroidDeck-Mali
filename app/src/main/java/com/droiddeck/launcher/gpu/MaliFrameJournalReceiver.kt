package com.droiddeck.launcher.gpu

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.system.Os
import com.droiddeck.launcher.runtime.LinuxRuntime
import com.droiddeck.launcher.session.SessionPhase
import com.droiddeck.launcher.session.SessionState
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class MaliFrameJournalReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        val app = context.applicationContext
        Thread({
            try {
                val directory = File(LinuxRuntime.logDir(app), "mali-frame-trace").apply {
                    check(isDirectory || mkdirs()) { "Cannot create journal directory" }
                    Os.chmod(path, 0x1c0)
                }
                val message = when (intent.getStringExtra("action")) {
                    "arm" -> {
                        check(!SessionState.running && SessionState.phase in setOf(SessionPhase.IDLE, SessionPhase.FAILED)) {
                            "Stop the session before arming a new journal"
                        }
                        directory.listFiles()?.filter { it.name == "armed" || it.name == "control" || it.name.startsWith("ready-") }
                            ?.forEach { check(it.delete()) { "Cannot clear journal control ${it.name}" } }
                        File(directory, "armed").writeText("")
                        "mali-journal-armed; start a session, then capture from its Terminal"
                    }
                    "disarm" -> {
                        for (name in listOf("armed", "control")) {
                            val file = File(directory, name)
                            check(!file.exists() || file.delete()) { "Cannot remove journal control $name" }
                        }
                        "mali-journal-disarmed"
                    }
                    "status" -> "mali-journal-status: armed=${File(directory, "armed").isFile} files=${directory.list()?.size ?: 0}"
                    "export" -> "mali-journal-export=${export(app, directory).path}"
                    else -> error("Unknown Mali journal action")
                }
                pending.resultCode = 1
                pending.resultData = message
            } catch (error: Throwable) {
                pending.resultCode = 0
                pending.resultData = "mali-journal-error=${error.message ?: error.javaClass.simpleName}"
            } finally {
                pending.finish()
            }
        }, "mali-frame-journal").start()
    }

    private fun export(context: Context, directory: File): File {
        val files = directory.listFiles().orEmpty().filter {
            it.isFile && (it.name.matches(Regex("trace-[0-9]+-[0-9]+-(gamescope|android)\\.jsonl")) ||
                it.name.matches(Regex("capture-[0-9]+\\.json")))
        }.sortedBy { it.name }
        check(files.sumOf { it.length() } <= 128L * 1024 * 1024) { "Journal export exceeds 128 MiB; clear old app logs first" }
        val output = File(checkNotNull(context.getExternalFilesDir("mali-frame-journal")), "frames.zip")
        val staged = File(output.parentFile, "frames.zip.staged")
        try {
            ZipOutputStream(staged.outputStream()).use { zip ->
                files.forEach { file ->
                    zip.putNextEntry(ZipEntry(file.name))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
            check(staged.renameTo(output)) { "Cannot publish journal archive" }
        } finally {
            staged.delete()
        }
        return output
    }
}
