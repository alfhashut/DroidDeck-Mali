package com.droiddeck.launcher.gpu

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class MaliProxyManifestTest {
    @get:Rule val temporary = TemporaryFolder()
    private lateinit var directory: File
    private lateinit var manifest: File
    private lateinit var library: File

    @Before fun setUp() {
        directory = temporary.newFolder("mali-vulkan")
        manifest = File(directory, "mali_proxy_icd.json")
        library = File(directory, "libdroiddeck_mali_proxy.so").apply { writeBytes(byteArrayOf(1)) }
    }

    private fun write(path: String = "./libdroiddeck_mali_proxy.so", format: String = "1.0.0", api: String = "1.0.0") {
        manifest.writeText(JSONObject().put("file_format_version", format)
            .put("ICD", JSONObject().put("library_path", path).put("api_version", api)).toString(2))
    }

    @Test fun knownDiagnosticRelativePathAndEquivalentPathsAreAccepted() {
        for (path in listOf("./libdroiddeck_mali_proxy.so", "././libdroiddeck_mali_proxy.so", library.absolutePath)) {
            write(path)
            assertEquals(library.canonicalFile, MaliNormalBroker.validateProxyManifest(manifest).canonicalFile)
        }
        // JSON key order and external whitespace are not part of validation.
        manifest.writeText(""" { "ICD": { "api_version": "1.0.0", "library_path": "./libdroiddeck_mali_proxy.so" }, "file_format_version": "1.0.0" } """)
        assertEquals(library.canonicalFile, MaliNormalBroker.validateProxyManifest(manifest).canonicalFile)
    }

    @Test fun wrongFormatApiAndJsonAreRejected() {
        write(format = "9.0.0")
        assertThrows(IllegalStateException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
        write(api = "1.2.0")
        assertThrows(IllegalStateException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
        manifest.writeText("{}")
        assertThrows(org.json.JSONException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
    }

    @Test fun otherLibrarySearchPathsAndEscapesAreRejected() {
        val outside = temporary.newFile("libdroiddeck_mali_proxy.so")
        File(directory, "other.so").writeBytes(byteArrayOf(1))
        for (path in listOf("./other.so", "libdroiddeck_mali_proxy.so", "../libdroiddeck_mali_proxy.so", outside.absolutePath)) {
            write(path)
            assertThrows(IllegalStateException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
        }
    }

    @Test fun missingLibraryAndSymlinkEscapeAreRejected() {
        write()
        check(library.delete())
        assertThrows(IllegalStateException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
        val outside = temporary.newFile("outside.so")
        Files.createSymbolicLink(library.toPath(), outside.toPath())
        assertThrows(IllegalStateException::class.java) { MaliNormalBroker.validateProxyManifest(manifest) }
    }
}
