# Checkpoint 1: glibc probe to Android Vulkan broker

This experiment starts only through a shell-protected debug Activity. Normal sessions and driver
selection are unchanged. There is no ICD, Gamescope patch, rendering, or fd-passing implementation.
The broker loads the phone's installed `/system/lib64/libvulkan.so`; no vendor libraries are bundled.

## Build

From the repository root, with an AArch64 glibc cross compiler installed:

```sh
tools/mali-vulkan/build-probe.sh
./gradlew assembleDebug
```

The script defaults to `aarch64-linux-gnu-gcc`; `CC` may name another AArch64 **glibc** compiler.
It produces a static executable at `app/src/main/assets/mali-vulkan/broker_probe`, ignored by Git.
The APK's existing native CMake build produces `libmalivulkan.so` using the Android NDK.
The APK workflow builds the probe independently of the preloads cache. Other existing APK build
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
app seccomp policy. The debug button now uses the existing proot execution path instead. This does
not remove Android's seccomp filter; success of this launch path must be checked on the device.
Expected success is the same real properties above with `probe via Linux/proot exit code=0` and
broker instance-destruction logs. If it fails, retain the proot output, exit code, and logcat.

## Protocol and validation

`protocol.h` defines protocol version 1: a 16-byte request/response header, a 12-byte response prefix,
and bounded 276-byte device records. All integers are explicitly encoded little-endian uint32;
names are bounded NUL-terminated byte strings. No native Vulkan structs, handles, pointers, or
`size_t` values are transmitted. Errors have a status and VkResult bits but no records. The limit
is 16 devices; overflow is an error, not silent truncation. Each connection serves one request.

The directory is mode 0700, the socket is mode 0600, and peer credentials must match the app UID.
Logcat distinguishes socket setup, loader loading, instance creation, enumeration, properties,
instance destruction, loader closure, and broker shutdown. No Vulkan object survives a query.

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
