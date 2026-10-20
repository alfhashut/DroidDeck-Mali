package com.droiddeck.launcher.session

import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient
import java.io.File

object SessionTerminal {
    private data class Launch(
        val command: Array<String>,
        val hostEnvironment: Array<String>,
        val workingDirectory: String,
        val generation: Int,
    )

    @Volatile private var launch: Launch? = null

    fun prepare(
        command: List<String>,
        hostEnvironment: Array<String>,
        workingDirectory: File,
        generation: Int,
    ) {
        launch = Launch(command.toTypedArray(), hostEnvironment.copyOf(), workingDirectory.path, generation)
    }

    fun create(client: TerminalSessionClient): TerminalSession? {
        val current = launch ?: return null
        if (!SessionState.running) return null
        return TerminalSession(
            current.command.first(), current.workingDirectory, current.command,
            current.hostEnvironment, 3000, client,
        ).also { it.mSessionName = "DroidDeck Linux shell" }
    }

    fun clear(generation: Int? = null) {
        val current = launch ?: return
        if (generation == null || current.generation == generation) launch = null
    }

    fun panvkProbeBuilder(guestScript: String): ProcessBuilder {
        val current = checkNotNull(launch) { "Start normal Mali Steam Launch first" }
        check(SessionState.running && SessionState.maliBackend && SessionState.mode == SessionService.MODE_STEAM) {
            "PanVK client proof requires a running normal Mali Steam session"
        }
        val shellArguments = when {
            current.command.takeLast(3) == listOf("/bin/bash", "-c", "cd \"\$HOME\" && exec /bin/bash -i") -> 3
            current.command.takeLast(2) == listOf("/bin/bash", "-i") -> 2
            else -> error("Unexpected prepared runtime terminal command")
        }
        return ProcessBuilder(current.command.dropLast(shellArguments) + listOf("/usr/bin/python3", "-B", guestScript)).apply {
            directory(File(current.workingDirectory))
            environment().clear()
            current.hostEnvironment.forEach { entry ->
                val separator = entry.indexOf('=')
                if (separator > 0) environment()[entry.substring(0, separator)] = entry.substring(separator + 1)
            }
            redirectErrorStream(true)
        }
    }
}
