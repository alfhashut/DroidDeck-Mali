package com.droiddeck.launcher.gpu

import android.content.Context
import android.system.Os
import android.util.Log
import com.droiddeck.launcher.runtime.GuestCommand
import com.droiddeck.launcher.runtime.LinuxRuntime
import java.io.File
import java.io.IOException

/** Opt-in Vulkan query experiments. Call off the main thread; normal sessions never start this. */
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

    /** Uses the guest's normal libvulkan.so.1, with the proxy selected for this command only. */
    @Synchronized
    fun runIcdTest(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val directory = path.parentFile
        for (name in listOf("vulkan_loader_test", "libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
            val file = File(directory, name)
            context.assets.open("mali-vulkan/$name").use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            }
            Os.chmod(file.path, if (name == "vulkan_loader_test") 0x1c0 else 0x180) // 0700 / 0600
        }
        val manifest = File(directory, "mali_proxy_icd.json")
        val binary = File(directory, "vulkan_loader_test")
        Log.i(TAG, "starting normal-loader ICD test via Linux/proot: ${binary.path}")
        val output = StringBuilder()
        // GuestCommand supplies a clean guest environment. /usr/bin/env sets these only
        // for this child; older loaders use VK_ICD_FILENAMES, newer ones VK_DRIVER_FILES.
        val command = listOf("/usr/bin/env", "VK_DRIVER_FILES=${manifest.path}",
            "VK_ICD_FILENAMES=${manifest.path}", "MALI_VULKAN_BROKER_SOCKET=${path.path}",
            "VK_LOADER_LAYERS_DISABLE=*", "VK_LOADER_DEBUG=error,warn,driver", binary.path)
        val code = GuestCommand.run(context, command) { line ->
            output.append(line).append('\n')
            Log.i(TAG, "ICD/proot: $line")
        }
        Log.i(TAG, "Vulkan ICD test via Linux/proot exit=$code")
        return "$output\nVulkan ICD test via Linux/proot exit code=$code"
    }

    /** Runs the actual staged DroidDeck Gamescope binary, without starting a session. */
    @Synchronized
    fun runGamescopeEnumeration(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val gamescope = "/usr/local/bin/gamescope"
        val bundled = context.assets.list("linuxfs/usr/local/bin")?.contains("gamescope") == true
        if (!bundled && !File(LinuxRuntime.rootDir(context), gamescope.removePrefix("/")).isFile) {
            return "DroidDeck Gamescope is not installed or bundled.\n" +
                "Build Gamescope 3.16.29 with patch 0113 and bundle it in the APK before retrying."
        }
        val directory = path.parentFile
        for (name in listOf("libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
            val file = File(directory, name)
            context.assets.open("mali-vulkan/$name").use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            }
            Os.chmod(file.path, 0x180) // 0600
        }
        val manifest = File(directory, "mali_proxy_icd.json")
        // GuestCommand stages the APK's Gamescope through the existing SessionFiles path.
        // The ICD overrides apply only to this diagnostic child, never to a normal session.
        val command = listOf("/usr/bin/env", "VK_DRIVER_FILES=${manifest.path}",
            "VK_ICD_FILENAMES=${manifest.path}", "MALI_VULKAN_BROKER_SOCKET=${path.path}",
            "VK_LOADER_LAYERS_DISABLE=*", "VK_LOADER_DEBUG=error,warn,driver",
            gamescope, "--vk-enumerate-only")
        Log.i(TAG, "starting Gamescope enumeration via Linux/proot: $gamescope --vk-enumerate-only")
        val output = StringBuilder()
        val code = GuestCommand.run(context, command) { line ->
            output.append(line).append('\n')
            Log.i(TAG, "Gamescope enumeration/proot: $line")
        }
        Log.i(TAG, "Gamescope Vulkan enumeration via Linux/proot exit=$code")
        val hint = if (code != 0 && output.contains("unrecognized option")) {
            "\nThis Gamescope binary lacks checkpoint 3. Bundle Gamescope 3.16.29 rebuilt with patch 0113 in the APK."
        } else ""
        return "$output\nGamescope Vulkan enumeration via Linux/proot exit code=$code$hint"
    }

    /** Runs the actual staged DroidDeck Gamescope binary, without starting a session. */
    @Synchronized
    fun runGamescopeDeviceTest(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val gamescope = "/usr/local/bin/gamescope"
        val bundled = context.assets.list("linuxfs/usr/local/bin")?.contains("gamescope") == true
        if (!bundled && !File(LinuxRuntime.rootDir(context), gamescope.removePrefix("/")).isFile) {
            return "DroidDeck Gamescope is not installed or bundled.\n" +
                "Build Gamescope 3.16.29 with patches 0113–0115 and bundle it in the APK before retrying."
        }
        val directory = path.parentFile
        for (name in listOf("libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
            val file = File(directory, name)
            context.assets.open("mali-vulkan/$name").use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            }
            Os.chmod(file.path, 0x180) // 0600
        }
        val manifest = File(directory, "mali_proxy_icd.json")
        // GuestCommand stages the APK's Gamescope through the existing SessionFiles path.
        // The ICD overrides apply only to this diagnostic child, never to a normal session.
        val command = listOf("/usr/bin/env", "VK_DRIVER_FILES=${manifest.path}",
            "VK_ICD_FILENAMES=${manifest.path}", "MALI_VULKAN_BROKER_SOCKET=${path.path}",
            "MALI_VULKAN_DEVICE_TEST=1", "VK_LOADER_LAYERS_DISABLE=*", "VK_LOADER_DEBUG=error,warn,driver",
            gamescope, "--vk-create-device-test")
        Log.i(TAG, "starting Gamescope device test via Linux/proot: $gamescope --vk-create-device-test")
        val output = StringBuilder()
        val code = GuestCommand.run(context, command) { line ->
            output.append(line).append('\n')
            Log.i(TAG, "Gamescope device test/proot: $line")
        }
        Log.i(TAG, "Gamescope Vulkan device test via Linux/proot exit=$code")
        val hint = if (code != 0 && output.contains("unrecognized option")) {
            "\nThis Gamescope binary lacks checkpoint 4B. Bundle Gamescope 3.16.29 rebuilt with patches 0113–0115 in the APK."
        } else ""
        return "$output\nGamescope Vulkan device test via Linux/proot exit code=$code$hint"
    }

    @Synchronized
    fun runGamescopeSubmitTest(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val gamescope = "/usr/local/bin/gamescope"
        val bundled = context.assets.list("linuxfs/usr/local/bin")?.contains("gamescope") == true
        if (!bundled && !File(LinuxRuntime.rootDir(context), gamescope.removePrefix("/")).isFile) {
            return "DroidDeck Gamescope is not installed or bundled.\n" +
                "Build Gamescope 3.16.29 with patches 0113–0116 and bundle it in the APK before retrying."
        }
        val directory = path.parentFile
        for (name in listOf("libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
            val file = File(directory, name)
            context.assets.open("mali-vulkan/$name").use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            }
            Os.chmod(file.path, 0x180) // 0600
        }
        val manifest = File(directory, "mali_proxy_icd.json")
        // GuestCommand stages the APK's Gamescope through the existing SessionFiles path.
        // The ICD overrides apply only to this diagnostic child, never to a normal session.
        val command = listOf("/usr/bin/env", "VK_DRIVER_FILES=${manifest.path}",
            "VK_ICD_FILENAMES=${manifest.path}", "MALI_VULKAN_BROKER_SOCKET=${path.path}",
            "MALI_VULKAN_SUBMIT_TEST=1", "VK_LOADER_LAYERS_DISABLE=*", "VK_LOADER_DEBUG=error,warn,driver",
            gamescope, "--vk-submit-test")
        Log.i(TAG, "starting Gamescope submit test via Linux/proot: $gamescope --vk-submit-test")
        val output = StringBuilder()
        val code = GuestCommand.run(context, command) { line ->
            output.append(line).append('\n')
            Log.i(TAG, "Gamescope submit test/proot: $line")
        }
        Log.i(TAG, "Gamescope Vulkan submit test via Linux/proot exit=$code")
        val hint = if (code != 0 && output.contains("unrecognized option")) {
            "\nThis Gamescope binary lacks checkpoint 4C. Bundle Gamescope 3.16.29 rebuilt with patches 0113–0116 in the APK."
        } else ""
        return "$output\nGamescope Vulkan submit test via Linux/proot exit code=$code$hint"
    }

    @Synchronized
    fun runCapabilities(context: Context): String {
        val path = socket ?: throw IOException("Broker is not running")
        if (!LinuxRuntime.isInstalled(context)) {
            return "Linux runtime is not installed or is being removed.\n" +
                "Install the Linux runtime in DroidDeck, then retry. The Android broker is still running."
        }
        val directory = path.parentFile
        for (name in listOf("capability_inventory", "libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
            val file = File(directory, name)
            context.assets.open("mali-vulkan/$name").use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            }
            Os.chmod(file.path, if (name == "capability_inventory") 0x1c0 else 0x180)
        }
        val manifest = File(directory, "mali_proxy_icd.json")
        val environment = listOf("/usr/bin/env", "VK_DRIVER_FILES=${manifest.path}",
            "VK_ICD_FILENAMES=${manifest.path}", "MALI_VULKAN_BROKER_SOCKET=${path.path}",
            "MALI_VULKAN_QUERY_CAPABILITIES=1", "VK_LOADER_LAYERS_DISABLE=*",
            "VK_LOADER_DEBUG=error,warn,driver")
        val output = StringBuilder()
        // Print the complete Android inventory separately: unsupported WSI extensions
        // must not be advertised by the query-only ICD just to make this diagnostic work.
        for (command in listOf(listOf(File(directory, "capability_inventory").path),
            listOf("/usr/local/bin/gamescope", "--vk-capabilities"))) {
            val code = GuestCommand.run(context, environment + command) { line ->
                output.append(line).append('\n')
                Log.i(TAG, "capabilities/proot: $line")
            }
            output.append("${command.first()} exit code=$code\n")
            if (code != 0) {
                output.append("Capability test failed. Bundle Gamescope with patches 0113 and 0114, then retry.\n")
                return output.toString()
            }
        }
        return output.toString()
    }

    @Synchronized
    fun stop() {
        if (socket == null) return
        nativeStop()
        socket = null
    }
}
