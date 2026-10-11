#!/usr/bin/env python3
"""Standalone phone probe: bundled ANGLE Vulkan pbuffer -> readback -> X11 CPU copy.

Never invoked by production startup. Requires a separately built native PanVK ICD.
No Vulkan memory export, Zink, Steam launch, or compositor configuration changes.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

P, I, U, L = C.c_void_p, C.c_int, C.c_uint, C.c_ulong
ATTR = C.c_ssize_t
SIZE = 64
ANGLE_COMMIT = '5d4df51d1d7d'
SOFTWARE = ('llvmpipe', 'softpipe', 'swrast', 'swiftshader', 'lavapipe', 'software')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def validate_renderer(vendor, renderer, version):
    combined = (vendor + ' ' + renderer + ' ' + version).lower()
    require(not any(name in combined for name in SOFTWARE), 'software renderer rejected')
    require('angle' in combined and 'vulkan' in renderer.lower(), 'ANGLE Vulkan not selected')
    require('mali-g52' in renderer.lower(), 'unexpected ANGLE physical device')
    require('panvk' in combined or 'panfrost' in combined, 'PanVK renderer not identified')
    require(ANGLE_COMMIT in version.lower(), 'ANGLE version differs from audited Steam bundle')


def validate_pixels(data):
    require(len(data) == SIZE * SIZE * 4, 'incomplete GPU readback')
    for x, y, expected in ((SIZE // 2, SIZE // 2, (255, 0, 0, 255)),
                           (1, SIZE - 2, (0, 0, 0, 255))):
        at = (y * SIZE + x) * 4
        require(tuple(data[at:at + 4]) == expected, f'GPU pixel mismatch at {x},{y}')


def native_elf(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    require(len(header) == 20 and header[:6] == b'\x7fELF\x02\x01'
            and struct.unpack_from('<H', header, 18)[0] == 183,
            f'expected little-endian AArch64 ELF: {path}')


def verify_loaded_libraries(args):
    paths = {line.split(maxsplit=5)[5] for line in
             Path('/proc/self/maps').read_text().splitlines()
             if len(line.split(maxsplit=5)) == 6 and line.split(maxsplit=5)[5].startswith('/')}
    mapped = {Path(path).resolve() for path in paths}
    for path in (args.egl, args.gles, args.panvk_library):
        require(Path(path).resolve() in mapped, f'required library not mapped: {path}')
    loaders = sorted(str(path) for path in mapped if path.name.startswith('libvulkan.so'))
    require(len(loaders) == 1, 'expected exactly one mapped Vulkan loader; inspect library paths')
    print('Vulkan loader=' + loaders[0], flush=True)
    print('PanVK library=' + str(Path(args.panvk_library).resolve()), flush=True)
    graphics = sorted(path for path in paths if any(name in path for name in
                      ('libvulkan', 'libEGL', 'libGLES', 'libgallium')))
    print('loaded graphics libraries:\n' + '\n'.join(graphics), flush=True)


def prepare(args):
    require(not any(os.environ.get(key) for key in
                    ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET')),
            'run inside the isolated Xwayland CLIENT environment, not Gamescope environment')
    require(os.environ.get('DISPLAY'), 'client DISPLAY required')
    root = Path(args.steam_root).resolve(strict=True)
    egl, gles = (Path(p).resolve(strict=True) for p in (args.egl, args.gles))
    require(egl.is_relative_to(root) and gles.is_relative_to(root) and egl.parent == gles.parent,
            'EGL/GLES must be paired libraries inside the installed Steam tree')
    icd, driver = (Path(p).resolve(strict=True) for p in (args.icd, args.panvk_library))
    require(icd.stat().st_size <= 1024 * 1024, 'oversized ICD manifest')
    manifest = json.loads(icd.read_text())
    require(isinstance(manifest, dict) and manifest.get('file_format_version') in ('1.0.0', '1.0.1'),
            'unexpected ICD format (expected 1.0.0 or 1.0.1)')
    entry = manifest.get('ICD')
    require(isinstance(entry, dict), 'ICD fields missing')
    require('library_arch' not in entry or entry['library_arch'] == '64',
            'ICD architecture must be string "64"')
    require(isinstance(entry.get('api_version'), str), 'ICD API version missing')
    library = Path(entry['library_path'])
    library = (library if library.is_absolute() else icd.parent / library).resolve(strict=True)
    require(library == driver and driver.name.startswith('libvulkan_panfrost')
            and '.so' in driver.name, 'manifest does not resolve to the explicit PanVK library')
    for path in (egl, gles, driver):
        native_elf(path)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(('PANVK_', 'MALI_VULKAN_', 'ANGLE_', 'VK_LOADER_')) or key in (
                'LIBGL_ALWAYS_SOFTWARE', 'MESA_LOADER_DRIVER_OVERRIDE', 'GALLIUM_DRIVER',
                'BL_VK_DRIVER', 'MESA_VK_VERSION_OVERRIDE', 'MESA_GL_VERSION_OVERRIDE',
                'MESA_GLES_VERSION_OVERRIDE', 'MESA_GLSL_VERSION_OVERRIDE',
                'MESA_EXTENSION_OVERRIDE', 'VK_INSTANCE_LAYERS', 'VK_LAYER_PATH',
                'VK_ADD_DRIVER_FILES', 'force_vk_devicename', 'force_vk_vendor',
                'LD_PRELOAD', 'LD_AUDIT'):
            env.pop(key, None)
    env.update(VK_DRIVER_FILES=str(icd), VK_ICD_FILENAMES=str(icd),
               PANVK_DXVK_COMPAT='0', PANVK_KBASE_DRI3='0',
               pan_force_enable_shader_atomics='false',
               pan_enable_vertex_pipeline_stores_atomics='false')
    # Set search paths before exec/dlopen. Parent/production environment is untouched.
    # Raw test artifacts bundle no dependencies; missing DSOs must come from the guest.
    env['LD_LIBRARY_PATH'] = ':'.join(dict.fromkeys((str(driver.parent), str(egl.parent),
        str(root / 'steamrtarm64/panorama'), '/usr/lib')))
    return env


def bind(lib, name, result, *args):
    function = getattr(lib, name)
    function.restype, function.argtypes = result, args
    return function


class Visual(C.Structure):
    _fields_ = [('ext', P), ('id', L), ('klass', I), ('red', L), ('green', L),
               ('blue', L), ('bits', I), ('entries', I)]


def present_cpu(data):
    """Copy verified CPU bytes, not an exported GPU object, into an ordinary X window."""
    x = C.CDLL('libX11.so.6')
    libc = C.CDLL(None)
    display = bind(x, 'XOpenDisplay', P, C.c_char_p)(None)
    require(display, 'cannot open client X11 display')
    window, image, gc = 0, None, None
    try:
        screen = bind(x, 'XDefaultScreen', I, P)(display)
        visual = bind(x, 'XDefaultVisual', C.POINTER(Visual), P, I)(display, screen)
        depth = bind(x, 'XDefaultDepth', I, P, I)(display, screen)
        require(sys.byteorder == 'little' and depth in (24, 32)
                and (visual.contents.red, visual.contents.green, visual.contents.blue)
                == (0xff0000, 0xff00, 0xff), 'unsupported X11 pixel layout')
        root = bind(x, 'XRootWindow', L, P, I)(display, screen)
        window = bind(x, 'XCreateSimpleWindow', L, P, L, I, I, U, U, U, L, L)(
            display, root, 0, 0, SIZE, SIZE, 0, 0, 0)
        require(window, 'X11 window creation failed')
        bind(x, 'XStoreName', I, P, L, C.c_char_p)(display, window, b'ANGLE Vulkan / Mali CPU-copy proof')
        bind(x, 'XMapRaised', I, P, L)(display, window)
        gc = bind(x, 'XCreateGC', P, P, L, L, P)(display, window, 0, None)
        require(gc, 'X11 GC creation failed')
        pixels = bytearray()
        # GL is bottom-up RGBA; XImage here is top-down BGRX.
        for y in range(SIZE - 1, -1, -1):
            for xx in range(SIZE):
                at = (y * SIZE + xx) * 4
                r, g, b, a = data[at:at + 4]
                pixels.extend((b, g, r, a))
        memory = bind(libc, 'malloc', P, C.c_size_t)(len(pixels))
        require(memory, 'CPU image allocation failed')
        C.memmove(memory, bytes(pixels), len(pixels))
        image = bind(x, 'XCreateImage', P, P, C.POINTER(Visual), U, I, I, P, U, U, I, I)(
            display, visual, depth, 2, 0, memory, SIZE, SIZE, 32, SIZE * 4)
        if not image:
            bind(libc, 'free', None, P)(memory)
            raise RuntimeError('XCreateImage failed')
        bind(x, 'XPutImage', I, P, L, P, P, I, I, I, I, U, U)(
            display, window, gc, image, 0, 0, 0, 0, SIZE, SIZE)
        bind(x, 'XSync', I, P, I)(display, 0)
        for xx, yy, rgb in ((SIZE // 2, SIZE // 2, 0xff0000), (1, 1, 0)):
            sample = bind(x, 'XGetImage', P, P, L, I, I, U, U, L, I)(
                display, window, xx, yy, 1, 1, 0xffffffff, 2)
            require(sample, 'X11 presentation readback failed')
            try:
                actual = bind(x, 'XGetPixel', L, P, I, I)(sample, 0, 0) & 0xffffff
                require(actual == rgb, 'X11 CPU-copy pixel mismatch')
            finally:
                bind(x, 'XDestroyImage', I, P)(sample)
        print('X11 CPU-copy pixels: verified; visually confirm the red triangle on phone', flush=True)
        time.sleep(2)  # Phone-only display time. Host tests never execute this path.
    finally:
        if image:
            bind(x, 'XDestroyImage', I, P)(image)  # Also frees malloc'd CPU pixels.
        if gc:
            bind(x, 'XFreeGC', I, P, P)(display, gc)
        if window:
            bind(x, 'XDestroyWindow', I, P, L)(display, window)
        bind(x, 'XCloseDisplay', I, P)(display)


def run(args):
    print(f'ICD={os.environ["VK_DRIVER_FILES"]}\nDISPLAY={os.environ["DISPLAY"]}', flush=True)
    print('mode=ANGLE Vulkan GLES2 pbuffer; presentation=verified CPU copy; no GPU FD export', flush=True)
    egl = C.CDLL(str(Path(args.egl).resolve()), mode=C.RTLD_GLOBAL)
    gl = C.CDLL(str(Path(args.gles).resolve()), mode=C.RTLD_GLOBAL)
    proc = bind(egl, 'eglGetProcAddress', P, C.c_char_p)

    def extension(name, result, *types):
        address = proc(name.encode())
        require(address, 'missing ANGLE entrypoint: ' + name)
        return C.CFUNCTYPE(result, *types)(address)

    query = bind(egl, 'eglQueryString', C.c_char_p, P, I)
    require(b'EGL_ANGLE_platform_angle_vulkan' in (query(None, 0x3055) or b'').split(),
            'bundled ANGLE does not advertise its Vulkan backend')
    attrs = (I * 3)(0x3203, 0x3450, 0x3038)
    display = extension('eglGetPlatformDisplayEXT', P, U, P, C.POINTER(I))(0x3202, None, attrs)
    require(display, 'ANGLE Vulkan display unavailable')
    initialized, surface, context = False, None, None
    try:
        require(bind(egl, 'eglInitialize', U, P, C.POINTER(I), C.POINTER(I))(display, None, None),
                'ANGLE Vulkan eglInitialize failed')
        initialized = True
        require(bind(egl, 'eglBindAPI', U, U)(0x30A0), 'GLES API unavailable')
        config, count = P(), I()
        attrs = (I * 13)(0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038)
        require(bind(egl, 'eglChooseConfig', U, P, C.POINTER(I), C.POINTER(P), I, C.POINTER(I))(
            display, attrs, C.byref(config), 1, C.byref(count)) and count.value, 'no RGBA8 GLES2 pbuffer config')
        surface = bind(egl, 'eglCreatePbufferSurface', P, P, P, C.POINTER(I))(
            display, config, (I * 5)(0x3057, SIZE, 0x3056, SIZE, 0x3038))
        context = bind(egl, 'eglCreateContext', P, P, P, P, C.POINTER(I))(
            display, config, None, (I * 3)(0x3098, 2, 0x3038))
        require(surface and context, 'pbuffer/context creation failed')
        require(bind(egl, 'eglMakeCurrent', U, P, P, P, P)(display, surface, surface, context),
                'eglMakeCurrent failed')
        get_string = bind(gl, 'glGetString', C.c_char_p, U)
        vendor, renderer, version = ((get_string(key) or b'').decode() for key in (0x1f00, 0x1f01, 0x1f02))
        print(f'GL_VENDOR={vendor}\nGL_RENDERER={renderer}\nGL_VERSION={version}', flush=True)
        validate_renderer(vendor, renderer, version)
        print('ANGLE backend=Vulkan; software renderer=none (validated)', flush=True)

        # Query the actual handles from ANGLE's device, not a separately selected Vulkan instance.
        device = ATTR()
        require(extension('eglQueryDisplayAttribEXT', U, P, I, C.POINTER(ATTR))(
            display, 0x322c, C.byref(device)), 'ANGLE device query unavailable')
        device_query = extension('eglQueryDeviceAttribEXT', U, P, I, C.POINTER(ATTR))
        values = []
        for key in (0x34a9, 0x34ab, 0x34ac, 0x34d1):
            value = ATTR()
            require(device_query(device.value, key, C.byref(value)) and value.value,
                    f'ANGLE Vulkan device attribute unavailable: {key:#x}')
            values.append(value.value)
        instance, physical, logical, gpa = values
        address = C.CFUNCTYPE(P, P, C.c_char_p)(gpa)(instance, b'vkGetPhysicalDeviceProperties')
        require(address, 'ANGLE Vulkan property function unavailable')
        properties = C.create_string_buffer(4096)
        C.CFUNCTYPE(None, P, P)(address)(physical, properties)
        api, driver_version, vendor_id, device_id, kind = struct.unpack_from('<5I', properties.raw)
        name = properties.raw[20:276].split(b'\0', 1)[0].decode()
        require(vendor_id == 0x13b5 and 'mali-g52' in name.lower() and kind != 4,
                'actual Vulkan device is not hardware Mali-G52')
        require(api >= (1 << 22 | 1 << 12), 'actual Vulkan device API is below ANGLE minimum 1.1')
        print(f'Vulkan device={name} vendor={vendor_id:#x} device={device_id:#x} API={api:#x}; VkDevice created', flush=True)
        verify_loaded_libraries(args)
        shaders = []
        program = 0
        try:
            for kind, text in ((0x8b31, b'attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}'),
                               (0x8b30, b'precision mediump float; void main(){gl_FragColor=vec4(1.,0.,0.,1.);}')):
                shader = bind(gl, 'glCreateShader', U, U)(kind)
                shaders.append(shader)
                source = C.c_char_p(text)
                bind(gl, 'glShaderSource', None, U, I, C.POINTER(C.c_char_p), P)(shader, 1, C.byref(source), None)
                bind(gl, 'glCompileShader', None, U)(shader)
                status = I()
                bind(gl, 'glGetShaderiv', None, U, U, C.POINTER(I))(shader, 0x8b81, C.byref(status))
                require(status.value, 'GLES shader compilation failed')
            program = bind(gl, 'glCreateProgram', U)()
            for shader in shaders:
                bind(gl, 'glAttachShader', None, U, U)(program, shader)
            bind(gl, 'glBindAttribLocation', None, U, U, C.c_char_p)(program, 0, b'p')
            bind(gl, 'glLinkProgram', None, U)(program)
            status = I()
            bind(gl, 'glGetProgramiv', None, U, U, C.POINTER(I))(program, 0x8b82, C.byref(status))
            require(status.value, 'GLES program link failed')
            bind(gl, 'glUseProgram', None, U)(program)
            bind(gl, 'glViewport', None, I, I, I, I)(0, 0, SIZE, SIZE)
            bind(gl, 'glClearColor', None, C.c_float, C.c_float, C.c_float, C.c_float)(0, 0, 0, 1)
            bind(gl, 'glClear', None, U)(0x4000)
            vertices = (C.c_float * 6)(-0.8, -0.8, 0.8, -0.8, 0, 0.8)
            bind(gl, 'glVertexAttribPointer', None, U, I, U, C.c_ubyte, I, P)(0, 2, 0x1406, 0, 0, vertices)
            bind(gl, 'glEnableVertexAttribArray', None, U)(0)
            bind(gl, 'glDrawArrays', None, U, I, I)(4, 0, 3)
            pixels = (C.c_ubyte * (SIZE * SIZE * 4))()
            # Synchronous GL readback proves completion before CPU reads/presentation.
            bind(gl, 'glReadPixels', None, I, I, I, I, U, U, P)(0, 0, SIZE, SIZE, 0x1908, 0x1401, pixels)
            require(bind(gl, 'glGetError', U)() == 0, 'GLES rendering/readback error')
            data = bytes(pixels)
            validate_pixels(data)
            print('GPU triangle readback: verified', flush=True)
            present_cpu(data)
        finally:
            if program:
                bind(gl, 'glDeleteProgram', None, U)(program)
            for shader in shaders:
                bind(gl, 'glDeleteShader', None, U)(shader)
    finally:
        if initialized:
            cleaned = bool(bind(egl, 'eglMakeCurrent', U, P, P, P, P)(display, None, None, None))
            if context:
                cleaned = bool(bind(egl, 'eglDestroyContext', U, P, P)(display, context)) and cleaned
            if surface:
                cleaned = bool(bind(egl, 'eglDestroySurface', U, P, P)(display, surface)) and cleaned
            cleaned = bool(bind(egl, 'eglTerminate', U, P)(display)) and cleaned
            require(cleaned, 'EGL teardown failed')
    print('probe render/copy/cleanup completed; Steam/CEF acceleration remains UNPROVEN', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('icd', 'panvk-library', 'egl', 'gles'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--steam-root', default=str(Path.home() / '.local/share/Steam'))
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.child:
            expected = prepare(args)
            require(all(os.environ.get(key) == expected[key] for key in
                        ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'PANVK_DXVK_COMPAT',
                         'PANVK_KBASE_DRI3', 'pan_force_enable_shader_atomics',
                         'pan_enable_vertex_pipeline_stores_atomics')),
                    'probe child environment does not select the validated inputs')
            run(args)
            return 0
        env = prepare(args)
        result = subprocess.run([sys.executable, '-B', str(Path(__file__).resolve()),
                                 *sys.argv[1:], '--child'], env=env, timeout=30)
        print(f'probe exit={result.returncode}')
        return result.returncode if result.returncode > 0 else (1 if result.returncode else 0)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f'ANGLE Vulkan probe FAILED: {error}', file=sys.stderr)
        print('probe exit=1', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
