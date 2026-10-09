package com.droiddeck.launcher.gpu

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
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
        val device = Button(this).apply { text = "Run Gamescope Vulkan device test"; isEnabled = false }
        val submit = Button(this).apply { text = "Run Gamescope Vulkan submit test"; isEnabled = false }
        val interopButtons = listOf(
            "Run Vulkan buffer memory test" to "--vk-buffer-memory-test",
            "Run Vulkan image memory test" to "--vk-image-memory-test",
            "Run Vulkan AHardwareBuffer test" to "--vk-ahb-test",
            "Run Vulkan AHB presentation test" to "--vk-ahb-present-test",
            "Run Gamescope renderer init test" to "--vk-gamescope-renderer-init-test",
            "Run Gamescope first frame test" to "--vk-gamescope-first-frame-test"
        ).map { (label, option) ->
            Button(this).apply { text = label; isEnabled = false } to option
        }
        val allButtons = listOf(probe, icd, gamescope, capabilities, device, submit) + interopButtons.map { it.first }
        val preview = SurfaceView(this).apply {
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(holder: SurfaceHolder) { SystemVulkanBroker.setDebugSurface(holder.surface) }
                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) { SystemVulkanBroker.setDebugSurface(holder.surface) }
                override fun surfaceDestroyed(holder: SurfaceHolder) { SystemVulkanBroker.setDebugSurface(null) }
            })
        }
        val stop = Button(this).apply { text = "Stop broker and close"; setOnClickListener { finish() } }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
            addView(probe); addView(icd); addView(gamescope); addView(capabilities); addView(device); addView(submit); interopButtons.forEach { addView(it.first) }; addView(stop); addView(status)
        }
        // Keep the consumer visible while the controls/logs scroll independently.
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(preview, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, (256 * resources.displayMetrics.density).toInt()))
            addView(ScrollView(this@SystemVulkanBrokerActivity).apply { addView(content) },
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        })
        interopButtons.forEach { (button, option) ->
            button.setOnClickListener {
                allButtons.forEach { it.isEnabled = false }
                status.text = "Running $option through the real Mali broker…"
                worker.execute {
                    val output = try { SystemVulkanBroker.runInteropTest(applicationContext, option) }
                    catch (t: Throwable) { Log.e("MaliVulkanBroker", "$option failed", t); "$option failed: $t" }
                    runOnUiThread {
                        if (!isDestroyed && !isFinishing) {
                            status.text = output
                            allButtons.forEach { it.isEnabled = true }
                        }
                    }
                }
            }
        }
        worker.execute {
            try {
                val socket = SystemVulkanBroker.start(applicationContext)
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = "Android broker listening\nSocket: $socket\n\nChoose the boundary probe, Vulkan ICD test, Gamescope enumeration, capability test, device test, or submit test. The Linux runtime must be installed. No normal session is started."
                        probe.isEnabled = true
                        icd.isEnabled = true
                        gamescope.isEnabled = true
                        capabilities.isEnabled = true
                        device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
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
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
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
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true; device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
        icd.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
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
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true; device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
        gamescope.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
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
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true; device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
        capabilities.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
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
                        status.text = output; probe.isEnabled = true; icd.isEnabled = true; gamescope.isEnabled = true; capabilities.isEnabled = true; device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
        device.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
            status.text = "Creating a real Android Vulkan device and obtaining a queue via Gamescope (no rendering)…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runGamescopeDeviceTest(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "device test failed", t)
                    "Gamescope Vulkan device test via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output
                        probe.isEnabled = true
                        icd.isEnabled = true
                        gamescope.isEnabled = true
                        capabilities.isEnabled = true
                        device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
        submit.setOnClickListener {
            probe.isEnabled = false
            icd.isEnabled = false
            gamescope.isEnabled = false
            capabilities.isEnabled = false
            device.isEnabled = false
            submit.isEnabled = false; interopButtons.forEach { it.first.isEnabled = false }
            status.text = "Submitting one event command to the real Mali queue and waiting on a fence (no rendering)…"
            worker.execute {
                val output = try {
                    SystemVulkanBroker.runGamescopeSubmitTest(applicationContext)
                } catch (t: Throwable) {
                    Log.e("MaliVulkanBroker", "submit test failed", t)
                    "Gamescope Vulkan submit test via Linux/proot failed: $t"
                }
                runOnUiThread {
                    if (!isDestroyed && !isFinishing) {
                        status.text = output
                        probe.isEnabled = true
                        icd.isEnabled = true
                        gamescope.isEnabled = true
                        capabilities.isEnabled = true
                        device.isEnabled = true; submit.isEnabled = true; interopButtons.forEach { it.first.isEnabled = true }
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        SystemVulkanBroker.setDebugSurface(null)
        // The shared worker orders teardown before any replacement Activity's startup.
        worker.execute { SystemVulkanBroker.stop() }
        super.onDestroy()
    }

    companion object {
        private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "mali-vulkan-debug") }
    }
}
