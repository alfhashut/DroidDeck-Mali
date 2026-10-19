# Steam X11 presentation investigation — 2026-10-11

This change adds diagnostics only. PanVK, stride 56, ANGLE Vulkan requests,
the client ICD, Gamescope's Mali proxy/broker, Android composition, advertised
capabilities and game/Proton/FEX support are unchanged. No heavy build ran.

## Real-phone findings

Phone: E68DS8U4IVYTKJXK. Session: `2026-10-11-26-steam`.
The dynamic receiver collected snapshots with no GL contexts or rendering,
no map/configure requests and no Composite redirection changes. Temporary
Composite/GLX references were checked and released on a separate connection.

The extension mapping is measured, not inferred from hard-coded opcodes:

| Extension | Major opcode | Present |
| --- | --- | --- |
| Composite | 142 | yes, version 0.4 |
| GLX | 149 | yes |
| MIT-SHM | 130 | yes |
| DRI3 | — | no |

Both WM_S0 and _NET_WM_CM_S0 have owner `0x200004`. The root is 1280×800,
depth 24. Depth 24 and depth 32 both have 32 bits per pixel; these quantities
must not be conflated.

| Window | Observed state | Visual/depth | Checked result |
| --- | --- | --- | --- |
| `0x2400034`, Valve output | Viewable, 1280×800 | `0x214` / 32 | Composite naming and GLX pixmap creation succeed; matching FBConfig 435 |
| `0x1400005`, browser top level | Viewable, 1280×800 | `0x218` / 32 | Composite naming and GLX pixmap creation succeed; matching FBConfig 439 |
| `0x1400012`, hidden browser | Unmapped, 1×1 | `0x218` / 32 | Composite NameWindowPixmap: BadMatch, opcode 142, minor 6 |
| `0x1400014`, hidden browser | Unmapped, 1×1 | `0x218` / 32 | Same BadMatch; current Steam AcquirePixmap error names this XID |
| `0x1400016`, hidden browser | Unmapped, 1×1 | `0x218` / 32 | Same BadMatch |

The successful GLX trials create references only: no context, texture binding,
draw or readback. Their FBConfig is selected by exact visual/depth and pixmap
drawable support. Steam's actual FBConfig/texture binding parameters remain
unknown; success does not prove all of its texture acquisition works.

Current `webhelper.txt`, at 18:31:32, records hidden MainMenu/QuickAccess
windows followed by Composite BadMatch, GLX BadDrawable, AcquirePixmap for
`0x1400014`, then GLXBadPixmap. The snapshot reproduces the first failing
operation on the same window. Composite naming an unmapped window has no
valid backing pixmap to return. The later invalid drawable/pixmap errors are
consistent with using that failed reference; tracing Steam's private calls
would be required to prove its exact internal error handling.

This is not evidence of a general depth-32/ARGB incompatibility. In particular,
the mapped browser and output windows pass with depth 32, including the same
browser visual as the hidden windows. Naming a viewable nested child can still
fail if that child itself is not redirected; map state alone is insufficient.

## Why GLX still appears

Steam's own `CCompositorGLThread::CreateOutputWindow`, `Loaded GL 4.6`,
`GLComposer` and SDL `mode: OpenGL` messages identify Valve's separate native
window compositor. CEF's requested ANGLE backend does not replace that layer.
The main webhelper maps runtime libGL/libGLX_mesa/libgallium; its browser/GPU
descendant also maps Steam's libEGL/libGLES. Library mappings alone do not
identify either layer's active renderer.

There is a material acceptance failure in the fresh session. At **18:31:37**,
`webhelper_gpu.txt` explicitly reports:

```text
GL implementation parts: (gl=egl-angle,angle=opengl)
Display type: ANGLE_OPENGL
GL_RENDERER: ANGLE (Mesa, llvmpipe (LLVM 22.1.8 128 bits), OpenGL 4.6 ...)
vulkan: disabled_off
```

The same launch has PanVK discovery, stride 56 and the JIT-init warning. Those
prove discovery/initialization, but do not override the active CEF renderer
report. This session does **not** satisfy ANGLE Vulkan/PanVK/Mali acceptance.
`failed to create drawable` is present, but does not identify its call site or
prove why CEF selected OpenGL. No fallback trigger is inferred from that line.
The JIT warning remains visible and is not designated the primary cause.

## Existing ingestion and narrow next fix

Gamescope's Mali X11 path has no AcquirePixmap or GLX texture acquisition.
`XWAYLAND_NO_GLAMOR=1` selects Xwayland SHM transport; the existing client-copy
import uploads that buffer through the unchanged proxy/broker output renderer.
The current session has actual SHM uploads and approximately 3.5–4 committed
frames/s. Changing Gamescope to SHM again cannot bypass Valve's earlier GLX
layer or turn the observed llvmpipe rendering into hardware rendering.

The narrow correction for the reproduced hidden-window failure belongs at
the acquisition lifecycle: skip texture acquisition while a browser window
is unmapped/unviewable, acquire after it becomes viewable and has a backing
pixmap, and reacquire after remap/resize. Do not pass a failed named-pixmap XID
into GLX. Steam owns this code; no supported DroidDeck switch implementing
that lifecycle has been established. Globally forcing hidden windows mapped,
inventing blank pixmaps, or suppressing errors would not be a justified fix.

For DroidDeck, first use the new lifecycle logs to check the existing
single-window configuration policy against the selected output window and
browser/popup windows. Only if it causes the visible window's mapping failure
should a follow-up restrict configuration/focus to the actual output window.
No such behavior change is included here. The measured mapped output currently
passes pixmap creation, so the hidden-popup errors alone do not justify it.

The desired architecture remains viable **if the client provides** a CPU
readback/presentation handoff from its hardware-rendered ANGLE Vulkan frame to
an X11/SHM buffer. Gamescope already accepts the latter and needs no PanVK WSI
or private DRI3 for that step. The standalone offscreen triangle proves only
the rendering part. No working real-Steam handoff bypassing Valve's GLX layer
has been demonstrated. A Gamescope-only format change cannot supply it.
Capture the active GPU process's initialization/backend-selection failure
before choosing that client integration change. Renderer validation is intact.

The earlier standalone X_GetImage BadMatch remains unresolved. XGetImage has
its own viewability and rectangle visibility requirements, so its error alone
does not establish the same request or root cause as Composite naming.

## Rerun and build requirements

From the repository root, leave the existing experimental Steam session open:

```sh
python3 -B tools/panvk/stage_phone_test.py --serial E68DS8U4IVYTKJXK --x11-diagnostics --run
```

No artifact, PanVK rebuild or APK rebuild is needed for this command. It uses
the installed development receiver and records a request-specific report in
`Download/DroidDeck/panvk-client-probe-<request>.log`. Completion means a snapshot
was collected; it never substitutes for the GPU proof's PASS marker. Log tails
are labelled as possibly historical: compare their timestamps to this launch.
A normal subsequent probe/configuration transfer restores the receiver's usual
entrypoint. The diagnostic does not change the persistent client option.

The optional observer changes and Gamescope patch `0125` require a later APK
rebuild to deploy; `0125` also requires rebuilding Gamescope. They only add
new-launch presentation logs, graphics library maps, and create/associate/map/
configure/first-buffer/presented diagnostics. They do not implement a fix.
No APK, Gamescope or PanVK build was performed for this investigation.

## Protocol references

- [X.Org Composite protocol](https://www.x.org/releases/current/doc/compositeproto/compositeproto.txt):
  NameWindowPixmap requires visible, redirected storage and that storage changes
  on remap/resize.
- [Xlib reference](https://x.org/releases/current/doc/libX11/libX11/libX11.pdf):
  XGetImage has separate window viewability/rectangle visibility constraints.

Phone reports and the code, rather than assumptions about GLX request numbers,
provide the findings above.
