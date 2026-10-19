# Installed Steam ARM64 CEF backend audit — 2026-10-11

## Finding and narrow correction

Installed Steam build **1791592380** supports **`-cef-use-vulkan`**, with help
text **`Use vulkan ANGLE backend`**. It conditionally emits
**`--use-angle=vulkan`** into the generated webhelper command.

DroidDeck had passed `-cef-use-angle=vulkan` and `-cef-use-gl=angle` directly
to Steam, but this build does not register those options. The strings are
absent from the inspected Steam executables/libraries, and neither requested
switch appears in the generated browser or GPU-process launch command.
Steam does not provide an automatic `-cef-<Chromium switch>` passthrough here.
The problem is the Steam option spelling, not placement after a URL or `--`.

The development PanVK branch now passes `-cef-use-vulkan`. Its launcher
requires that supported option and rejects the old/conflicting backend
options. It logs the complete exec argv before executing the original Steam
binary. The environment, atom stride, ICD and libraries are unchanged.
The existing Stop/restart/fallback paths remain covered by lightweight tests.
No webhelper interception/wrapper or binary modification is necessary.

Only the development Mali PanVK branch changes. C8A, non-Mali startup,
Gamescope, the Vulkan bridge, PanVK, X11 presentation and game paths are not
changed by this correction. No heavy build or launch with the new option was
performed during the read-only phone audit. Hardware UI acceptance is pending.

## Exact-build static evidence

These are copies of the installed phone files, not desktop Steam. Reading via
`adb run-as` was rejected because the app is not debuggable. The existing
development receiver read the binaries and wrote inspection copies to shared
storage; the installed files were preserved. Local disassembly used those
hash-identified copies.

| Installed file | SHA-256 |
| --- | --- |
| `steamrtarm64/steam` | `c6b97a641794ea0acadc2dcf8d3348bcc4e2b420cfca8dc973c398aac7ab8ba6` |
| `steamrtarm64/steamclient.so` | `178390e25dedbc0dcd69f9c02070418554dd329cddb166bb1ae169bd8fa7c018` |
| `steamrtarm64/steamwebhelper` | `e7dd5c4449796323565a61eeccdff38aff693b61091a22fa463de4ae6e6caff9` |
| `steamrtarm64/libcef.so` | `891d85f97231fcafa5763b7140039c5d3cbbb99f2a42ede08de5991abcbc3cdb` |

`steamclient.so` ELF file/virtual offsets for this build:

- `0x211b818`: `Use vulkan ANGLE backend`.
- `0x211b838`: `-cef-use-vulkan`.
- `0x211a598`: `--use-angle=vulkan`.
- At `0x40293c`–`0x402954`, the option/help strings are passed to
  `SteamStdLib::CCommandLineParam` for the object at `0x2d11eb0`.
- At `0xe47db8`–`0xe47dd4`, Steam looks up that registered option and branches
  to `0xe4a61c` when present.
- At `0xe4a61c`–`0xe4a654`, the generated command path adds the
  `--use-angle=vulkan` string.

Thus support is established by registration and code references, not merely
the presence of a string. `linuxarm64/steamclient.so` also contains the option
and generated switch. The inspected `steam`, `steamclient.so`, `steamui.so`,
`steamwebhelper`, `libcef.so` and webhelper scripts contain no `cef-use-angle`,
`cef-use-gl` or `no-linux-angle` option string. `libcef.so` does contain
Chromium's unprefixed `use-angle` and `use-gl` strings; that does not make the
invented Steam-prefixed spellings supported.

The installed ARM64 `steamwebhelper.sh` only sets DISPLAY if absent, sets
`LIBGL_KOPPER_DISABLE=true`, and executes `taskset 0x7c .../steamwebhelper "$@"`.
It contains no extra-argument environment passthrough or backend selection.
The observed main webhelper was launched directly by Steam.

## Live evidence and classification

Session `2026-10-11-27-steam`: Steam PID **18758**, main webhelper PID **18830**.
The complete NUL-separated `/proc/<pid>/cmdline` arrays were captured; an
abridged flag filter was not used for this audit. Steam's raw argv and fresh
`bootstrap_log.txt` agree on the unsupported request. Fresh `webhelper.txt`
at **18:44:33** agrees with the main helper's live argv and shows a generated
GPU command with `--type=gpu-process` but no `--use-angle` or `--use-gl`.

There is a process attribution caveat: none of the readable live cmdlines
contains `--type=gpu-process`. Fresh `webhelper_gpu.txt` identifies **18958**
as the GPU process in its PID-labelled sandbox message at **18:44:38**.
That same live PID's raw cmdline contains `--type=zygote`, with PPid **18855**.
This is consistent with a forked zygote child retaining its original argv;
the raw cmdline must not be rewritten or relabelled as a GPU-type argv capture.
The generated GPU launch command is independently recorded by Steam itself.

At **18:44:52**, the same session reports:

```text
GL implementation parts: (gl=egl-angle,angle=opengl)
Display type: ANGLE_OPENGL
GL_RENDERER: ANGLE (Mesa, llvmpipe ..., OpenGL 4.6 ...)
vulkan: disabled_off
```

The inherited PanVK environment is intact. These active renderer reports do
not meet hardware-renderer acceptance. No explicit later `--use-angle=gl` or
`--use-angle=opengl` override was found in the live/generated argv. The precise
default-selection internals are not needed to identify the missing request;
an exhaustive absence of other private settings is not claimed.

Classification: **(a) unsupported Steam option spellings** explains the loss.
**(b)** wrong argv placement is ruled out by the raw capture. **(c)** failed
translation of a supported option is not evidenced: the supported option was
never requested. **(d)** no later explicit OpenGL switch was observed.

## Read-only rerun and next phone acceptance

With the current experimental Steam session open, from the repository root:

```sh
python3 -B tools/panvk/stage_phone_test.py --serial E68DS8U4IVYTKJXK --cef-diagnostics --run
```

This dynamic helper needs no APK rebuild or raw artifact and makes no graphics
API calls. It records exact process argv, binary hashes/option strings and
bounded startup/backend log tails. No X11 diagnostic is invoked. Report tails
may include older launches: use PID, build ID and timestamps to distinguish
them. The completion marker is distinct from the GPU proof's PASS marker.

To deploy the source correction later, rebuild/install the APK containing
the updated session and client-launcher assets, then Stop/Launch normally with
the existing development PanVK option enabled. PanVK itself needs no rebuild.
Do not manually overwrite Steam or steamwebhelper. The audit alone does not
deploy the new launch option to the already-installed APK.

After that launch, require all of:

1. The exact Steam exec argv contains `-cef-use-vulkan`.
2. Main webhelper's live argv and the generated GPU-process launch command
   contain `--use-angle=vulkan`. Capture any live GPU cmdline that exposes its
   type; if it retains zygote argv, preserve the raw capture and correlate the
   GPU PID instead of claiming an argv acceptance pass.
3. Fresh `webhelper_gpu.txt` reports `angle=vulkan` / `ANGLE_VULKAN` and a
   Mali-G52/PanVK renderer, with no llvmpipe fallback.

Steam's supported option emits `--use-angle=vulkan`; it does not emit a
separate `--use-gl=angle` string at this branch. CEF already selects `egl-angle`
in the failing session. Confirm that implementation remains ANGLE in the next
fresh renderer report. Startup, environment inheritance and Vulkan discovery
alone remain insufficient for acceptance.

## Captured argv

The following commands are verbatim argument arrays rendered with shell
quoting. They describe the pre-correction session, not an acceptance result.

PID 18758:

```sh
/root/.local/share/Steam/steamrtarm64/steam -gamepadui -steamdeck -steamos3 -cef-enable-debugging -devtools-port 23382 -language english -clientbeta steamdeck_publicbeta -no-cef-sandbox -cef-force-gpu -cef-ozone-platform=x11 -cef-use-gl=angle -cef-use-angle=vulkan
```

PID 18830:

```sh
/root/.local/share/Steam/steamrtarm64/steamwebhelper -nocrashdialog -lang=en_US -cachedir=/root/.local/share/Steam/config/htmlcache -steampid=18758 -buildid=1791592380 -steamid=0 -logdir=/root/.local/share/Steam/logs -uimode=4 -startcount=0 -steamos3 -steamuniverse=Public -realm=Global -clientui=/root/.local/share/Steam/clientui -steampath=/root/.local/share/Steam/steamrtarm64/steam -launcher=0 --valve-enable-accessibility --valve-initial-threadpool-size=4 --valve-enable-site-isolation '--remote-allow-origins=*' --remote-debugging-address=127.0.0.1 --remote-debugging-port=23382 --enable-smooth-scrolling --ignore-gpu-blocklist --no-sandbox --password-store=basic --ignore-gpu-blocklist --log-file=/root/.local/share/Steam/logs/cef_log.txt --disable-quick-menu --disable-component-update --gaia-url=http://disabled.invalid --enable-features=PlatformHEVCDecoderSupport,V4L2VideoDecode --disable-features=WinRetrieveSuggestionsOnlyOnDemand,SpareRendererForSitePerProcess,BlockPromptsIfIgnoredOften,WebUsbDeviceDetection,ValveFFmpegAllowLowDelayHEVC
```

PID 18958:

```sh
/root/.local/share/Steam/steamrtarm64/steamwebhelper --type=zygote --no-zygote-sandbox --no-sandbox --enable-crash-reporter=, --change-stack-guard-on-fork=enable --enable-chrome-runtime --user-data-dir=/root/.local/share/Steam/config/htmlcache '--user-agent-product=Valve Steam Client' --buildid=1791592380 --steamid=0 --valve-initial-threadpool-size=4
```
