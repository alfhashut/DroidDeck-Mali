# C8A: ARM64 Steam client compatibility

Implementation awaits CI compilation and phone validation. No Steam UI, login,
network reachability, performance, or successful phone restart is claimed by host
command tests. This extends client/window compatibility while preserving the
phone-validated Mali Vulkan/AHB renderer.

## Existing production architecture

- `MainActivity.startSteamSession()` is the normal Play action. It opens
  `SessionActivity`, which owns the Android compositor surface and input.
- `SessionService.runSession()` stages the existing Linux runtime/session overlay,
  configures audio/controllers/storage, publishes Android network/DNS state,
  and starts `/usr/local/bin/droiddeck-session steam` through proot.
- The script starts Gamescope and re-enters the same script for its Steam client.
  `droiddeck-steam-install` downloads/checks Valve's existing ARM64 channel manifest
  and packages. Installation remains `$XDG_DATA_HOME/Steam` (normally
  `/root/.local/share/Steam`). The executable remains
  `$steam_root/steamrtarm64/steam`; its panorama/library search paths are retained.
- Existing `BL_STEAM_CHANNEL` selection (default `steamdeck_publicbeta`), language,
  `-gamepadui`, optional `-steamdeck -steamos3`, bootstrap/update exit-42 restart
  loop, stale-lock cleanup, Steam log collection and session-local CEF debugging
  guard remain in use. No new downloader, client bundle, channel or layout.
- Non-Mali launches keep Turnip/freedreno, Zink, ANGLE/Vulkan, ordinary Gamescope's
  Xwayland servers, game helpers and their existing commands.

## Why client compatibility is necessary

The previous Mali route changed Steam's requested mode to `mali-wayland` and
launched `gamescope --mali-interactive-client`. Its custom SHM server accepted one
raw Wayland surface and had no XWM/X11 display. Steam's existing startup requests
X11 (`GDK_BACKEND=x11`, `QT_QPA_PLATFORM=xcb`, `SDL_VIDEODRIVER=x11`,
`-cef-ozone-platform=x11`) and forced ANGLE/Vulkan.

The Mali proxy's `renderer_entries.def` contains compute/transfer operations; it
has no graphics-pipeline/draw/swapchain implementation. Pointing Steam/CEF at that
ICD cannot supply its GL/Vulkan UI requirements. Accordingly, **only the client**
uses Mesa software OpenGL, with ANGLE's GL backend. This is a compatibility choice,
not GPU feature spoofing. The compositor continues using real Mali Vulkan 1.1.

Patch `0124-mali-steam-xwayland-client.patch` uses Gamescope's existing pinned
wlroots Xwayland server and XWM; it does not introduce another session launcher.
The APIs were inspected at wlroots commit
`d783533489e1f75d6886c2ab5c5960090ef268f8`, pinned by Gamescope 3.16.29.
Xwayland's upstream `XWAYLAND_NO_GLAMOR=1` switch disables glamor/DRI3 and supplies
SHM buffers. A wlroots headless output advertises screen geometry/RandR only;
**no rendering or buffers are submitted to it**. Android output still uses the
existing CWaylandBackend/AHB path.

The XWM associates/maps/unmaps/destroys windows and selects one focused window for
SHM import, including bootstrap-to-CEF replacement and modal activation. Cursor
and unassociated raw surfaces cannot trip the old one-surface guard. Hidden
window callbacks are acknowledged without claiming their pixels were displayed.
This is a focused Steam UI session, not the full SteamOS multi-window/game
compositor: game overlays, arbitrary desktop windows and games are outside C8A.

## Selection, commands and environment

Device selection remains the existing vendor/device/API check: ARM vendor
`0x13b5`, Mali-G52 device `0x74021000`, truthful Vulkan API at least 1.1, no query
error. Steam semantics remain `MODE_STEAM`; `SessionState.maliBackend` selects
backend setup/teardown independently. The activity uses Android system Vulkan
instead of installing Turnip, disables unsupported HDR/frame generation, and
skips the Proton seed install. The explicit `mali-wayland` interactive workload
remains available unchanged.

Before (Mali normal Launch):

```text
gamescope --backend wayland --mali-wayland-session --expose-wayland -f \
  -W WIDTH -H HEIGHT -- /usr/local/bin/gamescope --mali-interactive-client
```

After (Mali Steam Launch):

```text
gamescope --backend wayland --mali-wayland-session --mali-xwayland \
  --expose-wayland -f -W WIDTH -H HEIGHT -- /usr/local/bin/droiddeck-session steam
```

The service uses `MaliNormalBroker.start()` unchanged: broker lease, listening
socket, inventory handshake and known proxy manifest staging/semantic validation.
The outer Gamescope process retains `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, broker
socket and renderer/AHB flags. Pacing defaults to 60 Hz; existing validated
30/60-Hz profiling overrides and all CP7P counters remain.

Xwayland readiness is required before forking the client script. The child gets
its X11 `DISPLAY` and nested Wayland socket. Only at that child boundary are proxy
ICD/broker/test variables and imported `BL_VK_DRIVER` removed. Client GL uses
`LIBGL_ALWAYS_SOFTWARE=1`, `MESA_LOADER_DRIVER_OVERRIDE=swrast`,
`GALLIUM_DRIVER=llvmpipe`, and `-cef-use-angle=gl`; the ordinary non-Mali branch
retains `-cef-use-angle=vulkan`. Missing Xwayland or
`/usr/lib/dri/swrast_dri.so` fails explicitly before Gamescope/Steam startup.
The runtime soname inventory contains Mesa GL/EGL, libgallium and LLVM, but only
the installed phone runtime can confirm the DRI module and CEF compatibility.
No dependencies are downloaded to conceal a missing graphics requirement.

No renderer/proxy/broker memory, staging, upload-range, descriptor, queue, timeline,
output-producer wait, AHB ownership, Android release or SYNC_FD implementation is
changed. The existing SHM import/render/Present block is checked byte-for-byte by
an extracted-source test. A static UI and a multi-minute Steam download no longer
trigger the diagnostic workload's 15-second no-frame watchdog in X11 mode;
client failure, Xwayland disconnect and Android disconnect remain failures.

## Network, input and shutdown

The common service path now supplies its existing `LinuxNetworkLinkComponent`:
Android ConnectivityManager state and DNS update the runtime's resolver/network
state before proot starts and through callbacks. The existing script starts the
system D-Bus, `droiddeck-netmanager` and login1 stand-ins. No namespace/VPN/proxy is
added. Logs identify this path without claiming Valve login is reachable.

Controllers use the existing fake evdev ring bindings, optional Deck hidraw/sysfs
bindings, SDL hints and libfakeinput preload. Pointer/touch-as-pointer/keyboard
use Android seat -> existing CWaylandBackend listeners -> Mali input queue ->
focused wlroots seat -> Xwayland -> Steam. The existing clipboard helper remains.
No new Steam Input system is introduced; touch scroll/SteamOS-specific root
properties and controller navigation need phone testing.

Normal Stop first asks Steam to exit through the existing `steam-stop` mailbox
and `steam -shutdown`. It then signals Gamescope using the existing Mali stop
helper, lets the real GPU/AHB drain finish, and stops the broker last. Startup
registers the proot identity under the stop lock; generic teardown cannot race
that Mali drain. Client helpers use their own process group, and Gamescope owns
Xwayland destruction. The script's sibling reaper is disabled for Mali so it
cannot kill the XWM prematurely. Release timeout failures still fail/retain owned
buffers. The runtime/client installation survives Stop for the next launch.

C8A disables automatic Proton/compatibility/seeder/extra-Proton requests, FEX
status/helpers, game mailbox/URL/autolaunch injection, added-game provisioning,
Lossless probing and Decky startup. Non-Mali branches retain their behavior.
This is no assertion that a game selected manually in Steam is supported.

## Diagnostics and validation

Session logs identify Steam/Mali selection, existing bootstrap start/result,
Gamescope/Xwayland/client supervisor/Steam PIDs, X11 readiness/focus, first actual
X11 surface presentation, Steam exit/restart and Gamescope/drain result. Window
titles and credentials are not logged by the new markers. A presented client
surface is **not** reported as verified Steam UI/sign-in. Android's first GPU
frame does not mark Steam repair as confirmed for this experimental backend.

Only lightweight checks are appropriate locally:

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_steam_launch test_normal_launch.NormalLaunchTests
```

These run fake executables and temporary local Unix sockets. They cover command
construction, dependency/precondition failures, existing channel/bootstrap and
update restart, game-helper exclusion, Steam failure propagation, stop mailbox,
second launch using the same fixture installation, ordinary ANGLE/Vulkan flags,
strict patch application and renderer boundary/source lifecycle contracts.
They do not run Android, Xwayland, real Steam, or compile Gamescope.

CI must compile the patched Gamescope/Kotlin/native code and package the updated
binary/overlay. On phone, install that CI APK with `adb install -r`, start normal
Steam, retain exact first-blocker logs, and verify the real Big Picture/sign-in UI
is visible and uncorrupted, navigation/keyboard/controller/network work, zero-copy
and release accounting remain correct, Stop is clean, and a second launch works
without reinstalling. CEF/GL missing-library/child-crash evidence is a blocker;
do not substitute a synthetic UI or declare PASS from process/PID markers.
