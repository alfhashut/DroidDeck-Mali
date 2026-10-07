package com.droiddeck.launcher.gpu

import android.content.Context
import android.system.Os
import android.util.Log
import com.droiddeck.launcher.runtime.GuestCommand
import com.droiddeck.launcher.runtime.LinuxRuntime
import java.io.File
import java.io.IOException

/** Checkpoint 1 only. Call explicitly, off the main thread; normal sessions never start this. */
object SystemVulkanBroker {
    private const val TAG = "MaliVulkanBroker"
    private var socket: File? = null

    @JvmStatic private external fun nativeStart(socketPath: String)
    @JvmStatic private external fun nativeStop()

    @Synchronized
    fun start(context: Context): String {
        socket?.let { return it.path }
        val directory = File(context.filesDir, "mali-vulkan")
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("Cannot create $directory")
        Os.chmod(directory.path, 0x1c0) // 0700; only this app can access the socket/probe.
        val path = File(directory, "broker.sock")
        System.loadLibrary("malivulkan")
        nativeStart(path.path)
        socket = path
        return path.path
    }

    /** Runs the glibc probe through the existing one-off Linux/proot command path. */
    @Synchronized
    fun runProbe(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val binary = File(path.parentFile, "broker_probe")
        context.assets.open("mali-vulkan/broker_probe").use { input ->
            binary.outputStream().use { output -> input.copyTo(output) }
        }
        Os.chmod(binary.path, 0x1c0)
        Log.i(TAG, "starting glibc probe via Linux/proot: ${binary.path} ${path.path}")
        val output = StringBuilder()
        val code = GuestCommand.run(context, listOf(binary.path, path.path)) { line ->
            output.append(line).append('\n')
            Log.i(TAG, "probe/proot: $line")
        }
        Log.i(TAG, "glibc probe via Linux/proot exit=$code")
        return "$output\nprobe via Linux/proot exit code=$code"
    }

    @Synchronized
    fun stop() {
        if (socket == null) return
        nativeStop()
        socket = null
    }
}
