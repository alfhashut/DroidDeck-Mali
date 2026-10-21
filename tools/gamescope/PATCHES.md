# gamescope patches carried by the app

Built by `.github/workflows/build-gamescope.yml` on top of the exact gamescope the Linux runtime
ships (3.16.29, Arch Linux ARM's package, same build options), and staged from the apk over
`/usr/local/bin/gamescope` at each session start - the hosted runtime image is never touched.
The binary's shared-library needs are checked against `runtime-sonames.txt`, the runtime's own
library list, before anything is published.

- `0002-steamcompmgr-fallback-appid-focus.patch` - Armada (armada-os/armada), verbatim.
- `0009-fix-arm64-steam-night-mode.patch` - Armada, verbatim: the ARM64 client packs the
  night-mode property differently; the slider did nothing.
- `0019-steamcompmgr-arm64-virtual-white.patch` - Armada, verbatim: the colour-temperature
  slider's (x, y) arrives as one 64-bit element from the ARM64 client; y is recovered from x.
- `0020-color-p3-red-is-wide-gamut.patch` - Armada, verbatim.
- `0100-realtime-queue-and-gamepad-cursor.patch` - this app, two of Armada's ported by hand onto
  3.16.29: realtime-priority Vulkan queues on request (`GAMESCOPE_FORCE_VULKAN_REALTIME=1`)
  without CAP_SYS_NICE, which proot can never have (a no-op on KGSL Turnip, which has a single
  submit-queue priority); and the gamepad-driven cursor sprite following the X pointer that XTest
  moves (it sat frozen). The X pointer is asked for only while a cursor image is drawn - every
  vblank while shown, every 50 ms while hidden for inactivity - since each ask is a blocking round
  trip to Xwayland on the paint thread; with no image, wlserver's position is used as upstream does.
- `0101-nested-wayland-cursor-routing.patch` - this app: `gamescope_cursor_composite=2` lets the
  nested Wayland session paint its cursor in the output using the real X pointer position; client
  cursor surfaces are cleared to prevent a duplicate at the host pointer, while Gamescope's original
  default host-cursor fallback is preserved when its no-keyboard-focus policy selects it. Empty
  client cursor images and Gamescope's hidden flag still suppress the painted cursor.
- `0110-wayland-backend-touch.patch` - this app: the nested Wayland backend bound only the host's
  pointer and keyboard, so a finger on the phone's screen never reached Steam. It now binds
  `wl_touch` too and hands each finger to wlserver's touch path (`wlserver_touchdown` / `motion` /
  `up`) - the one a Steam Deck's touchscreen drives - so what a touch does follows the client's
  touch mode (Steam's Big Picture sets Passthrough: a real touch, rows scroll under a finger).
  Finger ids are offset by one, since the nested pointer already moves wlserver's touch 0.
- `0111-wayland-pointer-warps-in-passthrough.patch` - this app: the nested pointer's motion goes to
  wlserver as touch 0, and in Passthrough (Big Picture's touch mode) a motion for a touch that is not
  down moves nothing - so in the app's touchpad mode the Steam client saw no hover and a click landed
  wherever the pointer had last been. The motion now always warps the real pointer as well
  (`bAlwaysWarpCursor`), which the other touch modes did already.
- `0112-restore-iconified-game-on-resume.patch` - this app: the Steam menu is an overlay that
  takes input without changing the focus window, and a fullscreen wine game minimizes itself when
  it loses input. gamescope only takes a window out of iconic when the focus window changes, and
  wine will not activate a window it believes iconic, so after Resume the game stayed minimized: a
  black screen with its small caption in the top-left corner (Titanfall 2, GE-Proton 11). The
  iconify request is remembered and the window goes back to NormalState before input returns to it,
  then focus is handed over again. `GAMESCOPE_RESTORE_FOCUS_WINDOW` on the root window asks for the
  same restore from outside (the session script's resume watcher).
- `0114-take-override-redirect-from-mapnotify.patch` - upstream (ValveSoftware/gamescope 3829340),
  verbatim; drop it once the runtime's gamescope includes it. Wine creates a game's window
  override-redirect and makes it managed before mapping it, which sends no X event. When gamescope
  read the window's attributes first, it kept the window as override-redirect and left it out of
  `GAMESCOPE_FOCUSABLE_WINDOWS`, so the client kept its loading screen over the running game
  (Skyrim SE, A Plague Tale: Innocence, on the games' own Xwayland). MapNotify now refreshes the flag.
- `0115-limiter-paces-per-window.patch` - this app: on a fixed-refresh output the frame limiter
  (Steam's Frame Limit) let a capped window's frames out only on every Nth refresh of a global grid,
  so a frame finished just after its slot waited a whole extra slot. At 30 on 60 Hz one picture was
  held for three or four refreshes - "small stops and ladders" with frame generation - where a game's
  own limiter slips such a frame by one refresh. The fixed-refresh branch now paces each window by
  time, as the VRR branch already did: its next frame goes out on the first refresh a capped interval
  after its last one, with half a refresh of tolerance so an on-time window keeps every Nth refresh.

Sixteen more of Armada's patches are DRM/lease/HDR-on-KMS work for a native display, which this
app's Wayland-hosted gamescope never reaches, or need a newer gamescope than the runtime has.

The Mali diagnostic patches follow all 0.5 patches in filename order. Their original
`mali-gpu-experiment` numbers are retained here for checkpoint documentation:

| Port file | Reference | Diagnostic |
| --- | --- | --- |
| `0200-vulkan-enumerate-only.patch` | 0113 | Instance/device inventory without renderer startup |
| `0201-vulkan-capabilities.patch` | 0114 | Real driver capability queries |
| `0202-vulkan-create-device-test.patch` | 0115 | Logical device and queue lifecycle |
| `0203-vulkan-submit-test.patch` | 0116 | Event command submission and fence completion |
| `0204-vulkan-memory-ahb-tests.patch` | 0117 | Independent buffer/image/AHB diagnostics |
| `0205-vulkan-gamescope-renderer-tests.patch` | 0118 | Explicit renderer initialization and first-frame diagnostics |
| `0206-vulkan-blit-input-proof.patch` | 0119 | Same-image pixel/readback proof |
| `0210-mali-persistent-session.patch` | 0120 | Persistent renderer/output ownership and nested SHM client diagnostics |
| `0211-mali-normal-wayland-session.patch` | 0121 | Normal single-client nested Wayland session with broker AHB-only output |
| `0212-mali-optimized-normal-resources.patch` | final 0121 | Persistent staging, completed watermark, upload dependencies, final normal performance controls |
| `0213-mali-frame-cost-attribution.patch` | 0122 | Exclusive frame/SHM/render costs and wait attribution |
| `0214-mali-upload-ring-ranges.patch` | 0123 | Modified upload ranges with conservative fallback |
| `0215-mali-steam-xwayland-client.patch` | 0124 | Dedicated Xwayland child group, SHM surfaces, isolated software Steam client |
| `0216-mali-steam-x11-presentation-diagnostics.patch` | 0125 | Opt-in X11 surface/presentation evidence |
| `0217-mali-frame-trace.patch` | 0126 | Bounded opt-in frame journal, explicit identities and dropped-record accounting |
| `0218-mali-staging-bulk-writes.patch` | 0127 | Coordinated 2 MiB persistent-staging write bound; shader bound unchanged |
| `0219-mali-desktop-first-frame-grace.patch` | local desktop follow-up | Explicit KDE child: bounded 90-second first-frame allowance, then original 15-second no-progress watchdog |

All 20 patches through 0211 retain their exact bytes. Phase 4 adds only 0212–0218,
regenerated against their complete preceding stack on Gamescope 3.16.29.
The old 0120 header insertion is omitted because 0204 already supplies the canonical
`interop_test_api.h`. The normal-session starting point is reference commit `7fd567ad`,
preserved in 0211. Its touch-down guard is merged into 0.5's existing touch tracking;
ordinary cursor, touch, and pointer-warp code remains in place. Overlay 0212 advances
that core to the final reference 0121 implementation at `3e1d9255`, retaining the ABI
headers already installed by Phase 3. Overlays 0213–0218 complete the coordinated
final implementation. Intermediate protocol headers in 0212 are advanced by 0218;
the complete stack matches the existing Phase 1 broker/proxy headers byte for byte.
These seven overlays must ship together.

Filename order is the application order: 0002, 0009, 0019, 0020, 0100, 0101, 0110,
0111, 0112, 0114, 0115, 0200–0206, 0210–0220. `test_phase3.py` applies the entire
stack with zero fuzz, checks that every Mali hunk has zero offset, verifies protocol/header
parity, and executes the actual output-pool state machine. The normal child receives
its nested Wayland socket without the compositor's private proxy/broker environment.
Producer SYNC_FD completion precedes publication; MB_NORMAL_END must acknowledge
Android releases before output resources are destroyed. Release failure retains
allocations and fails the session.

The native journal uses `DROIDDECK_FRAME_TRACE_DIR`, supplied by the current app's
private log directory. There is no Download-directory fallback. Its compiled-in root
override exists only for isolated host tests. No journal worker runs without an arm
marker at normal-session startup. Android callback identities are immutable snapshots;
callback threads never look up compositor buffers. Submit wall time is not GPU execution,
Android latch is not scanout, and any dropped records invalidate exact decomposition.

0215 retains Phase 3's unconditional removal of private proxy/broker variables from
every nested client. Only Gamescope uses the Mali proxy; Xwayland and ARM64 Steam use
Mesa software GL/SHM by default. Phase 5 adds an optional client-only PanVK/CEF
route outside the Gamescope bridge. Phase 6 adds a default-off app-specific shared
CPU staging marker; the existing private mapper and child-environment stripping
already support it, so the 27-patch stack remains byte-identical through 0218.
Shared staging changes only proxy/broker CPU transport and retains 2 MiB bulk
fallback. See `tools/mali-vulkan/SHARED_STAGING.md` for the opt-in and validation.

`check-diagnostics.sh` checks thirteen command markers plus the Xwayland and optimized
private-API markers, including in same-run artifact overlays. On the ARM64 builder it
requires independent diagnostics/persistent probes to fail with a missing ICD and
requires normal-session entry to fail without its isolated launch environment. This
smoke test does not prove real presentation. The existing cursor-routing test,
source fingerprint, same-run artifact overlay, and runtime SONAME audit remain.
The Phase 4 host runner exercises the actual small staging/registry/transport/journal
implementations and software launch script. Full Gamescope compilation and real
Mali/Android presentation remain CI/device checks. See `docs/development/mali-phase4.md`.

## KDE startup follow-up

0219 follows 0218 and preserves every previous patch byte. It changes only the
normal-session watchdog state and its logging. A first-frame allowance of 90
seconds requires `BL_MALI_DESKTOP=1`, a `droiddeck-session lxqt` child, and no
`--mali-xwayland`. After the first successful Present, the original 15-second
no-progress limit applies. Connections, surface creation and commits do not reset
that timer. Interactive diagnostics retain 15 seconds from child startup; Steam's
existing Xwayland watchdog exemption is unchanged. Fatal child/display errors and
normal Stop remain immediate independent of this timer.

0219 did not supply desktop protocols. Device logs subsequently proved alias
survival and the full 90-second allowance, with no commits or frames. The captured
parent registry contained only wl_shm, wl_compositor and wl_seat.

## KWin 6.7.5 host compatibility

`0220-mali-kwin-host-protocols.patch` follows 0219; all 28 previous patch files
remain byte-identical. Only the explicitly selected KDE child uses this server
extension. Normal Gamescope's wlserver uses wlroots' xdg-shell and pointer-constraint
handlers but is bypassed by the custom Mali entry point. 0220 reuses wlroots 0.20.2
handlers for subcompositor, xdg-shell (v3), single-pixel-buffer, viewporter, pointer
constraints and presentation-time. wlroots performs role, configure/ack, viewport,
subsurface synchronization and resource-lifetime validation. Pointer constraints
remain inactive by compositor policy; no locked/confined event is fabricated.

KWin submits its software-rendered desktop as subsurfaces above a single-pixel
background. wlroots' xdg/subsurface scene helpers maintain those trees and cursor
surfaces; its Pixman renderer composes them into a persistent SHM frame. That
frame enters the existing Mali Vulkan texture upload and presentation path. The
CPU composition step is desktop-only and needs device performance validation.
Pixman and the SHM allocator are compiled unconditionally by the pinned wlroots;
the existing renderers=[]/allocators=[] build options are preserved.

The headless output provides wl_output metadata only: it is never committed or
used to invent presentation timing. Sampled client feedback is forwarded from
the real outer Android wp_presentation feedback, preserving clock, timestamps,
sequence and flags except ZERO_COPY, which is cleared for CPU composition.
Discard and shutdown discard pending feedback; outstanding feedback is bounded.
No broker protocol, Vulkan synchronization or AHB/release ordering changes.

The builder runs `mali_desktop_protocols` and `mali_desktop_unconfigured` alongside
`cursor_routing`. These small Wayland client/server tests exercise the actual
wlroots handlers, configure/ack and popup lifecycle, single-pixel buffers, SHM
subsurface composition, synchronized/desynchronized commits, viewport cropping,
pointer-constraint object requests, frame callbacks, actual outer-event forwarding,
discard/unmap/disconnect and invalid pre-configure buffer rejection. They allocate
a dynamic socket with wayland-0 occupied. They model Android presentation events;
they do not prove a real KDE frame or device rendering. See
`docs/development/mali-phase3.md` for device evidence and remaining checks.
