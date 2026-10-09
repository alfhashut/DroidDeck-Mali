package com.droiddeck.launcher.gpu

import android.content.Context
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import com.droiddeck.launcher.wayland.CompositorHost
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** One normal session's broker lease. Startup/drain belong on the service worker. */
object MaliNormalBroker {
    @Synchronized fun start(context: Context): List<String> {
        check(CompositorHost.isStarted) { "Android compositor must start before the Mali broker" }
        check(MaliSessionSelection.supported(VulkanInfo.query())) { "GPU is outside the validated Mali-G52 scope" }
        // nativeStartWithSurface starts a worker. Its return does not guarantee
        // that the compositor has finished Vulkan initialization and bound its socket.
        val displaySocket = File(context.filesDir, ".wayland-rt/wayland-0")
        val deadline = SystemClock.elapsedRealtime() + 10000
        fun displayReady(): Boolean = runCatching {
            if (!OsConstants.S_ISSOCK(Os.stat(displaySocket.path).st_mode)) return@runCatching false
            LocalSocket().use { connection ->
                connection.connect(LocalSocketAddress(displaySocket.path, LocalSocketAddress.Namespace.FILESYSTEM))
            }
            true
        }.getOrDefault(false)
        while (!displayReady()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Android Wayland compositor socket did not become ready" }
            Thread.sleep(20)
        }
        val socket = SystemVulkanBroker.startNormal(context)
        try {
            // A socket file is not readiness: execute the existing native inventory request.
            LocalSocket().use { connection ->
                connection.connect(LocalSocketAddress(socket, LocalSocketAddress.Namespace.FILESYSTEM))
                // connect creates the underlying FD before Android accepts socket options.
                connection.soTimeout = 3000
                val request = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                    .putInt(0x564d4444).putInt(1).putInt(1).putInt(0).array()
                connection.outputStream.write(request)
                fun read(size: Int): ByteBuffer {
                    val bytes = ByteArray(size); var at = 0
                    while (at < size) {
                        val n = connection.inputStream.read(bytes, at, size - at)
                        if (n <= 0) throw IOException("Mali broker readiness reply ended early")
                        at += n
                    }
                    return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
                }
                val header = read(16)
                check(header.int == 0x564d4444 && header.int == 1 && header.int == 1) { "Invalid Mali broker readiness header" }
                val length = header.int
                check(length in 12..(12 + 16 * 276)) { "Invalid Mali broker readiness length" }
                val reply = read(length)
                check(reply.int == 0 && reply.int == 0) { "Mali broker cannot initialize system Vulkan" }
                val count = reply.int
                check(count in 1..16 && length == 12 + count * 276) { "Mali broker returned no valid GPU" }
                var found = false
                repeat(count) { index ->
                    reply.position(12 + index * 276 + 256)
                    val vendor = reply.int; val device = reply.int; val api = reply.int
                    if (vendor == 0x13b5 && device == 0x74021000 && api >= ((1 shl 22) or (1 shl 12))) found = true
                }
                check(found) { "Mali broker did not return the selected Mali-G52 Vulkan 1.1 device" }
            }
            val directory = File(context.filesDir, "mali-vulkan")
            for (name in listOf("libdroiddeck_mali_proxy.so", "mali_proxy_icd.json")) {
                val target = File(directory, name); val staged = File(directory, "$name.staged")
                try {
                    context.assets.open("mali-vulkan/$name").use { input -> staged.outputStream().use { input.copyTo(it) } }
                    Os.chmod(staged.path, 0x180)
                    check(staged.renameTo(target)) { "Cannot stage Mali proxy $name" }
                } finally { staged.delete() }
            }
            val manifest = File(directory, "mali_proxy_icd.json")
            val library = validateProxyManifest(manifest)
            val elf = library.inputStream().use { input -> ByteArray(20).also { check(input.read(it) == it.size) } }
            check(elf[0] == 0x7f.toByte() && elf[1] == 'E'.code.toByte() && elf[2] == 'L'.code.toByte() && elf[3] == 'F'.code.toByte() &&
                elf[4] == 2.toByte() && elf[5] == 1.toByte() && elf[18] == 0xb7.toByte() && elf[19] == 0.toByte()) { "Mali proxy asset is not an AArch64 ELF library" }
            return MaliSessionSelection.environment(manifest.path, socket)
        } catch (error: Throwable) {
            runCatching { SystemVulkanBroker.stopNormal() }.exceptionOrNull()?.let { error.addSuppressed(it) }
            throw error
        }
    }

    internal fun validateProxyManifest(manifest: File): File {
        val metadata = JSONObject(manifest.readText())
        check(metadata.getString("file_format_version") == "1.0.0") { "Unexpected Mali proxy manifest format version" }
        val icd = metadata.getJSONObject("ICD")
        check(icd.getString("api_version") == "1.0.0") { "Unexpected Mali proxy manifest API version" }
        val directory = checkNotNull(manifest.parentFile).canonicalFile
        val library = File(directory, "libdroiddeck_mali_proxy.so")
        val path = icd.getString("library_path")
        val reference = File(path)
        val resolved = (if (reference.isAbsolute) reference else File(directory, path)).canonicalFile
        // The diagnostic manifest uses ./libdroiddeck_mali_proxy.so. Resolve it
        // beside the manifest; a bare soname would instead use loader search paths.
        check(path.contains('/') && resolved == library.canonicalFile && resolved.parentFile == directory && library.isFile) {
            "Mali proxy manifest must reference the staged libdroiddeck_mali_proxy.so"
        }
        return library
    }

    @Synchronized fun stop(): Boolean = SystemVulkanBroker.stopNormal()
}
