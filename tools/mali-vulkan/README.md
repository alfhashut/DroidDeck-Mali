# Mali Vulkan query checkpoints

This experiment starts only through a shell-protected debug Activity. Normal sessions and driver
selection are unchanged. Checkpoint 1 uses a direct socket probe; checkpoint 2 adds a query-only
ICD selected explicitly for its test command. There is no Gamescope patch, rendering, or fd passing.
The broker loads the phone's installed `/system/lib64/libvulkan.so`; no vendor libraries are bundled.

## Build

From the repository root, with an AArch64 glibc cross compiler installed:

```sh
tools/mali-vulkan/build-probe.sh
tools/mali-vulkan/build-icd.sh
./gradlew assembleDebug
```

The script defaults to `aarch64-linux-gnu-gcc`; `CC` may name another AArch64 **glibc** compiler.
It produces a static executable at `app/src/main/assets/mali-vulkan/broker_probe`, ignored by Git.
The APK's existing native CMake build produces `libmalivulkan.so` using the Android NDK.
The APK workflow builds both tests and the proxy ICD independently of the preloads cache. Other existing APK build
prerequisites still apply. A build without the probe asset can start the broker, but its Run button
reports the missing asset instead of running a different binary.

## Invoke on the phone

Install the resulting APK using the usual compatible-signature installation process. Then:

```sh
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -s MaliVulkanBroker:I '*:S'
```

For a package variant, replace the package before `/`; the Activity's class name stays the same.
The Activity requires `android.permission.DUMP`, like the existing shell debug trampoline. It has
no launcher entry and does not start Steam, Gamescope, the compositor, or a Linux session.

Install the Linux runtime through DroidDeck if it is not already installed. Keep the debug screen
open and press **Run probe via Linux/proot**. A missing or removing runtime produces a clear message
without launching the probe; the Android broker remains available.

The Activity extracts the same bundled glibc executable into its private directory and runs it
through `GuestCommand.run()`, the existing one-off Linux/proot launcher used by package/import tools.
That launcher prepares the runtime, supplies `PROOT_LOADER` and the usual proot environment, and
binds the app's private files at the same paths inside the guest. Both the executable and
`broker.sock` therefore keep their existing paths. The Bionic broker stays in the Android process.
There is no root/su/adb invocation in application code and no automatic fallback to direct execution.

The client still does not link or load Vulkan. The screen and logcat show merged stdout/stderr,
the device count, all six returned properties, and `probe via Linux/proot exit code`. `driverVersion`
is printed as raw vendor-specific data, not interpreted as a universal major/minor/patch version.

The broker remains available for repeated queries. Press **Stop broker and close**, or Back, to
destroy the Activity and stop it. Teardown interrupts blocked socket reads, joins the broker thread,
and removes the socket. It waits for an in-progress vendor Vulkan call rather than cancelling it.
An Android process kill also closes descriptors; the next start removes its stale socket pathname.

After the Run button has extracted the probe, it may also be invoked from an existing Linux shell
running as this app's UID. Use the exact app-private socket path displayed on screen; normally:

```sh
/data/user/0/com.droiddeck.launcher/files/mali-vulkan/broker_probe \
  /data/user/0/com.droiddeck.launcher/files/mali-vulkan/broker.sock
```

An ordinary adb shell cannot access the app-private socket as the app's UID. Use the debug screen's
Run button for this test; no root, su, or session environment overrides are needed. Repeat the Run
action to check that the broker remains available, then close the screen and check the cleanup logs.

## Real-device result and launch-path follow-up

Checkpoint 1 was validated on Android 13 / MT6769V/CZ (mt6768): the exact glibc probe, launched
externally under the app UID, reached the Bionic broker and reported:

```text
deviceName: Mali-G52
vendorID: 0x000013b5
deviceID: 0x74021000
apiVersion: 1.1.131
driverVersion: 109051904
deviceType: 1
```

Direct execution as an Android-app child exited 159 / SIGSYS, consistent with the inherited Android
app seccomp policy. The existing proot execution path was subsequently validated on the real phone:
the same properties were returned with `probe via Linux/proot exit code=0`. Both debug actions use
this path; they do not remove or bypass Android's seccomp filter.

## Protocol and validation

`protocol.h` defines protocol version 1: a 16-byte request/response header, a 12-byte response prefix,
and bounded 276-byte device records. All integers are explicitly encoded little-endian uint32;
names are bounded NUL-terminated byte strings. No native Vulkan structs, handles, pointers, or
`size_t` values are transmitted. Errors have a status and VkResult bits but no records. The limit
is 16 devices; overflow is an error, not silent truncation. Each connection serves one request.

The directory is mode 0700, the socket is mode 0600, and peer credentials must match the app UID.
Logcat distinguishes socket setup, loader loading, instance creation, enumeration, properties,
instance destruction, loader closure, and broker shutdown. Version-1 instances are destroyed before
the response. Version-2 instances live until explicit destruction, disconnect, or broker shutdown.

Host-side protocol tests compile the probe against local glibc and use synthetic socket responses:

```sh
python3 tools/mali-vulkan/test_probe.py
bash -n tools/mali-vulkan/build-probe.sh
git diff --check
```

These tests verify framing, fragmentation, bounds, and error handling. They do **not** prove the
Android/Bionic boundary. Checkpoint acceptance requires running on the phone: the real glibc
probe must report Mali-G52 properties matching Android's Vulkan query and exit 0, with corresponding
broker enumeration/cleanup logs. No successful-device output is fabricated on failure.

For the same tests under AArch64 emulation, set `HOST_CC` to an AArch64 glibc compiler and
`PROBE_RUNNER=qemu-aarch64` when invoking `test_probe.py`.

## Checkpoint 2: normal Vulkan loader and query-only ICD

The separate **Run Vulkan ICD test** button runs:

```text
vulkan_loader_test -> guest libvulkan.so.1 -> libdroiddeck_mali_proxy.so
                  -> Unix socket -> Bionic broker -> Android system Vulkan
```

`loader_test.c` opens normal `libvulkan.so.1` and resolves Vulkan entry points from that loader.
It never opens or links the proxy directly. `icd_proxy.c` is a glibc shared library, packaged as an
asset alongside `vulkan_loader_test` and `mali_proxy_icd.json`, not as an Android JNI library.
The NDK-built broker continues to live in the Android process. No phone/vendor libraries are copied.

Install an AArch64 glibc cross compiler and Vulkan development headers (`gcc-aarch64-linux-gnu`,
`libc6-dev-arm64-cross`, `libvulkan-dev` on the Ubuntu CI runner). `build-icd.sh` defaults to those
headers under `/usr/include`; alternatively set `VULKAN_HEADERS` to a Vulkan-Headers include directory.
It builds a dynamic glibc ICD/test, so the installed Linux runtime must provide the matching glibc
and its normal `libvulkan.so.1`. No glibc Vulkan library is needed at cross-link time: the application
loads the runtime's loader with `dlopen`. Build both scripts above before building/installing the APK.

Install the APK with the same signing key as the currently installed app, install the Linux runtime
if necessary, and open the same shell-protected Activity. For the test-key-signed local debug build:

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -s MaliVulkanBroker:I '*:S'
```

Press **Run probe via Linux/proot** to recheck checkpoint 1, then **Run Vulkan ICD test**. Both actions
capture merged stdout/stderr and exit code in the screen and logcat. Missing runtime/assets/loader,
socket/protocol errors, and Vulkan failures are reported as errors; no device output is fabricated.
The ICD test supplies `VK_DRIVER_FILES` and legacy `VK_ICD_FILENAMES`, both naming only the private
proxy manifest, plus `MALI_VULKAN_BROKER_SOCKET`. These variables affect only that guest command.
Implicit layers are disabled via `VK_LOADER_LAYERS_DISABLE=*` on loaders supporting that variable.
The normal freedreno/Turnip manifest and every normal session's environment are unchanged.

Expected phone acceptance output (host mocks alone cannot validate this):

```text
glibc libvulkan.so.1: vkCreateInstance=0
Vulkan physical-device count: 1
Device 0
deviceName: Mali-G52
vendorID: 0x000013b5
deviceID: 0x74021000
apiVersion: 1.1.131
driverVersion: 109051904
deviceType: 1
Destroyed Vulkan instance
Vulkan ICD test via Linux/proot exit code=0
```

Check broker logs for system-loader loading, successful instance creation, count/properties, and
destruction/loader closure. Repeat both buttons, then **Stop broker and close** to verify teardown.
For manual invocation inside an existing guest shell under the app UID, after the button stages assets:

```sh
test_dir=/data/user/0/com.droiddeck.launcher/files/mali-vulkan
env VK_DRIVER_FILES="$test_dir/mali_proxy_icd.json" \
    VK_ICD_FILENAMES="$test_dir/mali_proxy_icd.json" \
    MALI_VULKAN_BROKER_SOCKET="$test_dir/broker.sock" \
    VK_LOADER_LAYERS_DISABLE='*' "$test_dir/vulkan_loader_test"
```

### Scope and loader interface

This is an experimental instance-query subset, **not a conformant Vulkan rendering driver**.
The manifest and instance-version query cap the instance API at 1.0. No ICD instance/device extensions,
layers, rendering features, queues, or memory heaps are advertised. Real device properties, including
the Android driver's API version and all core limits/UUID/sparse-property fields, are returned intact.
That hardware API version is not a claim that the proxy implements Vulkan 1.1.

The ICD negotiates loader interface versions 2–5, exposes `vk_icdGetInstanceProcAddr` and
`vk_icdGetPhysicalDeviceProcAddr`, and initializes the first dispatch word of each local instance
and physical device with `VK_LOADER_DATA`/loader magic. Driver implementations remain private
to the shared library; only those three loader entry points are exported.
See the [Khronos driver interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md).

The [loader's required core dispatch slots](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/loader/generated/vk_loader_extensions.c)
include feature/format/queue/memory/sparse queries, device-extension enumeration, `vkGetDeviceProcAddr`,
and `vkCreateDevice`, even when the application only enumerates devices. Their proxy implementations
report empty capabilities, return no device functions, or explicitly reject device creation/image formats.
They never request those operations from Android. No logical Vulkan device can be created.
Unknown functions and WSI entry points return NULL. Application extensions, application `pNext`
structures, instance flags, and custom allocation callbacks are rejected. Only loader-private instance
bookkeeping nodes are ignored; their callbacks/pointers never cross to Bionic.

### Version-2 protocol and lifetimes

Version 1 remains wire compatible. Version 2 uses a connection-owned Android instance: CREATE receives
the requested API version; LIST returns bounded, connection-local IDs starting at 1; PROPERTIES accepts
one ID and returns full core properties; DESTROY acknowledges cleanup and closes the connection.
No Android handle, pointer, native struct layout, or native `size_t` is serialized.
`properties_fields.def` specifies each scalar; `properties.h` encodes uint32/int32/IEEE binary32 fields
as four little-endian bytes and uint64/size fields as eight, with bounded names and a 16-byte UUID.
Decoding range-checks size values. Malformed framing/IDs and count overflow fail explicitly.

The ICD caches immutable properties before publishing local physical-device objects, so transport
failures can be reported by `vkEnumeratePhysicalDevices` rather than an unreportable void query.
Repeated enumeration preserves local object identity until instance destruction. Each instance owns
a distinct socket and mutex; the broker permits up to 16 simultaneous same-UID connections, allowing
multiple instances and the original probe to coexist. Disconnects and stop clean up remote instances;
stop interrupts idle reads and joins workers without cancelling vendor Vulkan calls.

### Host validation

With host glibc, a C compiler, Vulkan headers, JDK JNI headers, binutils, and normal `libvulkan.so.1`:

```sh
python3 tools/mali-vulkan/test_probe.py
python3 tools/mali-vulkan/test_icd.py
bash -n tools/mali-vulkan/build-probe.sh tools/mali-vulkan/build-icd.sh
git diff --check
```

Set `VULKAN_HEADERS` if headers are elsewhere, and `JAVA_HOME` if the JDK is not `/usr/lib/jvm/default`.
These new host tests use the actual broker source with test-only mocked Android loader/JNI functions.
They cover the real host Vulkan loader, negotiation/export symbols, dispatch magic, unsupported operations,
stable local handles, full property round trips, version-1 regression, concurrent instances, disconnect/stop,
invalid IDs/API versions, malformed/fragmented responses, and backend errors/cleanup. Mock properties
are explicitly named `HOST TEST ONLY`; this is not evidence that phone checkpoint 2 has passed.
