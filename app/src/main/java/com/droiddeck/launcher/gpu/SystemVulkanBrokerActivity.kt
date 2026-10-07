package com.droiddeck.launcher.gpu

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.concurrent.Executors

/** Shell-only experimental screen, protected by android.permission.DUMP in the manifest. */
class SystemVulkanBrokerActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val status = TextView(this).apply {
            text = "Mali Vulkan query checkpoints\nStarting Android broker…"
            setTextIsSelectable(true)
        }
        val probe = Button(this).apply { text = "Run probe via Linux/proot"; isEnabled = false }
        val icd = Button(this).apply { text = "Run Vulkan ICD test"; isEnabled = false }
        val gamescope = Button(this).apply { text = "Run Gamescope Vulkan enumeration test"; isEnabled = false }
        val capabilities = Button(this).apply { text = "Run Gamescope Vulkan capability test"; isEnabled = false }
        val stop = Button(this).apply { text = "Stop broker and close"; setOnClickListener { finish() } }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
            addView(probe); addView(icd); addView(gamescope); addView(capabilities); addView(stop); addView(status)
        }
        setContentView(ScrollView(this).apply { addView(content) })
        worker.execute {
            try {
                val socket = SystemVulkanBroker.start(applicationContext)
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = "Android broker listening\nSocket: $socket\n\nChoose the boundary probe, Vulkan ICD test, Gamescope enumeration, or capability test. The Linux runtime must be installed. No normal session is started."
                        probe.isEnabled = true
                        icd.isEnabled = true
                        gamescope.isEnabled = true
                        capabilities.isEnabled = true
                    }
                }
            } catch (t: Throwable) {
                Log.e("MaliVulkanBroker", "broker start failed", t)
                runOnUiThread { if (!isDestroyed) status.text = "Broker start failed: $t" }
            }
        }
        probe.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            status.text = "Querying Android Vulkan from the glibc probe via Linux/proot…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runProbe(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "probe failed", t)
                    "Probe via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true
                    }
                }
            }
        }
        icd.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            status.text = "Querying Android Vulkan through glibc libvulkan.so.1 and the proxy ICD via Linux/proot…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runIcdTest(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "ICD test failed", t)
                    "Vulkan ICD test via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true
                    }
                }
            }
        }
        gamescope.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            status.text = "Enumerating Android Vulkan from Gamescope via Linux/proot (no renderer/backend initialization)…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runGamescopeEnumeration(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "Gamescope enumeration failed", t)
                    "Gamescope Vulkan enumeration via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true
                    }
                }
            }
        }
        capabilities.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            status.text = "Querying real Android Vulkan capabilities via Gamescope (no logical device/backend)…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runCapabilities(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "capability test failed", t)
                    "Gamescope Vulkan capability test via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        // The shared worker orders teardown before any replacement Activity's startup.
        worker.execute { SystemVulkanBroker.stop() }
        super.onDestroy()
    }

    companion object {
        private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "mali-vulkan-debug") }
    }
}
