package com.droiddeck.launcher.gpu

import android.content.Context
import android.system.Os
import android.util.Log
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

    /** Runs a separate, statically linked glibc process; no Linux session or Vulkan client library. */
    @Synchronized
    fun runProbe(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        val binary = File(path.parentFile, "broker_probe")
        context.assets.open("mali-vulkan/broker_probe").use { input ->
            binary.outputStream().use { output -> input.copyTo(output) }
        }
        Os.chmod(binary.path, 0x1c0)
        val process = ProcessBuilder(binary.path, path.path).redirectErrorStream(true).start()
        try {
            val output = process.inputStream.bufferedReader().use { it.readText() }
            val code = process.waitFor()
            Log.i(TAG, "glibc probe exit=$code\n$output")
            return "$output\nprobe exit code=$code"
        } finally {
            process.destroy()
        }
    }

    @Synchronized
    fun stop() {
        if (socket == null) return
        nativeStop()
        socket = null
    }
}
