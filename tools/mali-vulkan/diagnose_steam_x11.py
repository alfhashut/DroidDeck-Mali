#!/usr/bin/env python3
"""Development X11 snapshot. No contexts, rendering, window changes or redirection.

The only created objects are temporary Composite/GLX pixmap references on our
own connection. A GLX trial uses a reported matching FBConfig, not Steam's
unknown FBConfig. It cannot establish which renderer Steam actually uses.
"""
import ctypes as C
import json
import os
from pathlib import Path
import re

U = C.c_ulong
I = C.c_int
P = C.c_void_p


class Visual(C.Structure):
    _fields_ = [('ext_data', P), ('visualid', U), ('class_', I),
                ('red_mask', U), ('green_mask', U), ('blue_mask', U),
                ('bits_per_rgb', I), ('map_entries', I)]


class Attributes(C.Structure):
    _fields_ = [(n, I) for n in ('x', 'y', 'width', 'height', 'border_width', 'depth')] + [
        ('visual', C.POINTER(Visual)), ('root', U), ('class_', I),
        ('bit_gravity', I), ('win_gravity', I), ('backing_store', I),
        ('backing_planes', U), ('backing_pixel', U), ('save_under', I),
        ('colormap', U), ('map_installed', I), ('map_state', I),
        ('all_event_masks', C.c_long), ('your_event_mask', C.c_long),
        ('do_not_propagate_mask', C.c_long), ('override_redirect', I), ('screen', P)]


class Error(C.Structure):
    _fields_ = [('type', I), ('display', P), ('resourceid', U), ('serial', U),
                ('error_code', C.c_ubyte), ('request_code', C.c_ubyte), ('minor_code', C.c_ubyte)]


class Format(C.Structure):
    _fields_ = [('depth', I), ('bits_per_pixel', I), ('scanline_pad', I)]


class VisualInfo(C.Structure):
    _fields_ = [('visual', C.POINTER(Visual)), ('visualid', U), ('screen', I),
                ('depth', I), ('class_', I), ('red_mask', U), ('green_mask', U),
                ('blue_mask', U), ('colormap_size', I), ('bits_per_rgb', I)]


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, list(args)
    return fn


def emit(label, value):
    print(label + '=' + json.dumps(value, sort_keys=True), flush=True)


class X11:
    def __init__(self):
        self.x = C.CDLL('libX11.so.6')
        specs = [
            ('XOpenDisplay', P, C.c_char_p), ('XCloseDisplay', I, P),
            ('XDefaultScreen', I, P), ('XRootWindow', U, P, I),
            ('XSync', I, P, I), ('XFree', I, P),
            ('XSetErrorHandler', P, P), ('XGetErrorText', I, P, I, C.c_char_p, I),
            ('XQueryExtension', I, P, C.c_char_p, C.POINTER(I), C.POINTER(I), C.POINTER(I)),
            ('XGetWindowAttributes', I, P, U, C.POINTER(Attributes)),
            ('XListPixmapFormats', C.POINTER(Format), P, C.POINTER(I)),
            ('XQueryTree', I, P, U, C.POINTER(U), C.POINTER(U), C.POINTER(C.POINTER(U)), C.POINTER(C.c_uint)),
            ('XInternAtom', U, P, C.c_char_p, I), ('XGetSelectionOwner', U, P, U),
            ('XGetWindowProperty', I, P, U, U, C.c_long, C.c_long, I, U,
             C.POINTER(U), C.POINTER(I), C.POINTER(U), C.POINTER(U), C.POINTER(C.POINTER(C.c_ubyte))),
            ('XGetGeometry', I, P, U, C.POINTER(U), C.POINTER(I), C.POINTER(I),
             C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)),
            ('XFreePixmap', I, P, U)]
        for name, result, *args in specs:
            bind(self.x, name, result, *args)
        self.d = self.x.XOpenDisplay(None)
        if not self.d:
            raise RuntimeError('cannot open the live client DISPLAY')
        self.errors = []
        self.handler = C.CFUNCTYPE(I, P, C.POINTER(Error))(self.error)
        self.old_handler = self.x.XSetErrorHandler(C.cast(self.handler, P))
        self.screen = self.x.XDefaultScreen(self.d)
        self.root = self.x.XRootWindow(self.d, self.screen)

    def error(self, _, event):
        e = event.contents
        message = C.create_string_buffer(256)
        self.x.XGetErrorText(self.d, e.error_code, message, len(message))
        self.errors.append(dict(error=message.value.decode(errors='replace'), code=e.error_code,
                                opcode=e.request_code, minor=e.minor_code,
                                resource=hex(e.resourceid), serial=e.serial))
        return 0

    def checked(self, call, *args):
        self.x.XSync(self.d, 0)
        self.errors.clear()
        value = call(*args)
        self.x.XSync(self.d, 0)
        return value, list(self.errors)

    def attributes(self, window):
        a = Attributes()
        ok, errors = self.checked(self.x.XGetWindowAttributes, self.d, window, C.byref(a))
        if not ok or errors:
            return None, errors
        return a, []

    def children(self, window):
        root, parent, count = U(), U(), C.c_uint()
        children = C.POINTER(U)()
        ok, errors = self.checked(self.x.XQueryTree, self.d, window, C.byref(root),
                                 C.byref(parent), C.byref(children), C.byref(count))
        try:
            return list(children[:min(count.value, 512)]) if ok and not errors else []
        finally:
            if children:
                self.x.XFree(children)

    def property(self, window, name):
        atom = self.x.XInternAtom(self.d, name.encode(), 1)
        if not atom:
            return None
        actual, count, remaining, fmt = U(), U(), U(), I()
        data = C.POINTER(C.c_ubyte)()
        ok, errors = self.checked(self.x.XGetWindowProperty, self.d, window, atom, 0, 256, 0, 0,
                                 C.byref(actual), C.byref(fmt), C.byref(count), C.byref(remaining), C.byref(data))
        try:
            if ok or errors or not data:
                return None
            if fmt.value == 32:
                return list(C.cast(data, C.POINTER(U))[:count.value])
            if fmt.value == 8:
                return C.string_at(data, count.value).decode(errors='replace').replace('\0', ' ').strip()
        finally:
            if data:
                self.x.XFree(data)

    def geometry(self, drawable):
        root, x, y = U(), I(), I()
        width, height, border, depth = (C.c_uint() for _ in range(4))
        ok, errors = self.checked(self.x.XGetGeometry, self.d, drawable, C.byref(root), C.byref(x),
                                 C.byref(y), C.byref(width), C.byref(height), C.byref(border), C.byref(depth))
        return dict(root=hex(root.value), width=width.value, height=height.value,
                    depth=depth.value) if ok and not errors else dict(errors=errors)

    def close(self):
        self.x.XSync(self.d, 0)
        self.x.XSetErrorHandler(self.old_handler)
        self.x.XCloseDisplay(self.d)


def pixmap_trial(x, composite, glx, configs, window, a):
    pixmap, errors = x.checked(composite.XCompositeNameWindowPixmap, x.d, window)
    emit('CompositeNameWindowPixmap', dict(window=hex(window), pixmap=hex(pixmap), errors=errors))
    # Failed NameWindowPixmap returns an XID too, but it names no resource.
    # Never feed that XID into GLX and manufacture a misleading error cascade.
    if errors or not pixmap:
        return
    try:
        emit('named-pixmap-geometry', x.geometry(pixmap))
        matches = [(config, info) for config, info in configs if
                   info['visual'] == a.visual.contents.visualid and info['depth'] == a.depth
                   and (info['drawable_type'] or 0) & 2]
        emit('matching-GLX-pixmap-FBConfigs', [info for _, info in matches])
        if glx and matches:
            config, info = matches[0]
            attrs = (I * 1)(0)
            drawable, errors = x.checked(glx.glXCreatePixmap, x.d, config, pixmap, attrs)
            emit('glXCreatePixmap-trial', dict(drawable=hex(drawable), fbconfig=info['id'], errors=errors,
                                             note='no context/bind/render; Steam FBConfig unknown'))
            if drawable and not errors:
                _, errors = x.checked(glx.glXDestroyPixmap, x.d, drawable)
                if errors:
                    emit('glXDestroyPixmap-errors', errors)
    finally:
        _, errors = x.checked(x.x.XFreePixmap, x.d, pixmap)
        if errors:
            emit('XFreePixmap-errors', errors)


def glx_configs(x):
    try:
        g = C.CDLL('libGLX.so.0')
        bind(g, 'glXGetFBConfigs', C.POINTER(P), P, I, C.POINTER(I))
        bind(g, 'glXGetVisualFromFBConfig', C.POINTER(VisualInfo), P, P)
        bind(g, 'glXGetFBConfigAttrib', I, P, P, I, C.POINTER(I))
        bind(g, 'glXCreatePixmap', U, P, P, U, C.POINTER(I))
        bind(g, 'glXDestroyPixmap', None, P, U)
        count = I()
        configs, errors = x.checked(g.glXGetFBConfigs, x.d, x.screen, C.byref(count))
        if errors:
            emit('GLX-FBConfig-errors', errors)
        result = []
        try:
            for index in range(min(count.value, 4096)) if configs else ():
                config = configs[index]
                visual = g.glXGetVisualFromFBConfig(x.d, config)
                if not visual:
                    continue
                try:
                    info = dict(visual=visual.contents.visualid, depth=visual.contents.depth)
                    for name, attr in [('id', 0x8013), ('drawable_type', 0x8010),
                                       ('bind_rgb', 0x20D0), ('bind_rgba', 0x20D1), ('texture_targets', 0x20D3)]:
                        value = I()
                        status = g.glXGetFBConfigAttrib(x.d, config, attr, C.byref(value))
                        info[name] = value.value if status == 0 else None
                    result.append((config, info))
                finally:
                    x.x.XFree(visual)
        finally:
            if configs:
                x.x.XFree(configs)
        return g, result
    except (OSError, AttributeError) as error:
        emit('GLX-query-unavailable', str(error))
        return None, []


def steam_evidence(client):
    """Bounded observations; historical log tails are explicitly labelled."""
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            argv = process.joinpath('cmdline').read_bytes().decode(errors='replace').split('\0')
            if not any(Path(arg).name == 'steamwebhelper' for arg in argv):
                continue
            env = dict(item.split('=', 1) for item in process.joinpath('environ').read_bytes()
                       .decode(errors='replace').split('\0') if '=' in item)
            if env.get('DISPLAY') != client['DISPLAY']:
                continue
            maps = process.joinpath('maps').read_text()
            libraries = sorted({row.split(maxsplit=5)[5] for row in maps.splitlines()
                                if len(row.split(maxsplit=5)) == 6 and re.search(
                                    r'lib(?:EGL|GLES|GLX|GLdispatch|GL\.|vulkan|gallium)|swrast|llvmpipe', row, re.I)})
            emit('live-webhelper', dict(pid=int(process.name),
                                       flags=[a for a in argv if a.startswith(('--type=', '--use-'))],
                                       libraries=libraries,
                                       env={k: env.get(k) for k in ('PANVK_ATOM_STRIDE', 'VK_DRIVER_FILES',
                                                                  'ANGLE_DEFAULT_PLATFORM')}))
        except (OSError, ValueError):
            continue
    logs = Path(client.get('HOME', '/root')) / '.local/share/Steam/logs'
    for name in ('webhelper.txt', 'steamwebhelper.log', 'cef_log.txt', 'webhelper_gpu.txt'):
        try:
            with (logs / name).open('rb') as stream:
                stream.seek(max(0, os.fstat(stream.fileno()).st_size - 65536))
                lines = stream.read(65536).decode(errors='replace').splitlines()
            selected = [line for line in lines if re.search(
                r'AcquirePixmap|BadMatch|BadDrawable|GLXBadPixmap|failed to create drawable|'
                r'CCompositorGLThread|GLComposer|Loaded GL |Created window|WasHidden|'
                r'GL_RENDERER|ANGLE|Vulkan|Mali-G52|llvmpipe|atom stride|\[KFAULT\]|KBASE_IOCTL_MEM_JIT_INIT|'
                r'ERROR|failed|GPU process|gpu-process', line, re.I)]
            emit('Steam-log-tail-may-include-prior-launches', dict(file=name, lines=[line[:2048] for line in selected[-60:]]))
        except OSError:
            continue


def run():
    from steam_client_environment import client_environment
    # Use only the proven session's X11 connection coordinates. No graphics
    # selectors copied, no PanVK/ANGLE DSOs loaded, no renderer validation relaxed.
    client = client_environment()
    for key in ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR'):
        if key in client:
            os.environ[key] = client[key]
        else:
            os.environ.pop(key, None)
    emit('X11-presentation-diagnostics', dict(display=client['DISPLAY'], contexts=0, rendering=False,
                                           window_changes=False, redirect_changes=False))
    steam_evidence(client)
    x = X11()
    try:
        for name in ('Composite', 'GLX', 'DRI3', 'MIT-SHM', 'DAMAGE', 'RENDER'):
            opcode, event, error = I(), I(), I()
            present = x.x.XQueryExtension(x.d, name.encode(), C.byref(opcode), C.byref(event), C.byref(error))
            emit('extension', dict(name=name, present=bool(present), opcode=opcode.value,
                                   first_event=event.value, first_error=error.value))
        emit('root', x.geometry(x.root))
        for name in ('WM_S' + str(x.screen), '_NET_WM_CM_S' + str(x.screen)):
            atom = x.x.XInternAtom(x.d, name.encode(), 1)
            emit('selection-owner', dict(name=name, window=hex(x.x.XGetSelectionOwner(x.d, atom)) if atom else None))
        count = I()
        formats = x.x.XListPixmapFormats(x.d, C.byref(count))
        try:
            emit('pixmap-formats', [dict(depth=f.depth, bpp=f.bits_per_pixel, pad=f.scanline_pad)
                                    for f in formats[:count.value]])
        finally:
            if formats:
                x.x.XFree(formats)
        composite = C.CDLL('libXcomposite.so.1')
        bind(composite, 'XCompositeQueryVersion', I, P, C.POINTER(I), C.POINTER(I))
        bind(composite, 'XCompositeNameWindowPixmap', U, P, U)
        major, minor = I(), I()
        if not composite.XCompositeQueryVersion(x.d, C.byref(major), C.byref(minor)):
            raise RuntimeError('Composite version query failed')
        emit('Composite-version', [major.value, minor.value])
        glx, configs = glx_configs(x)
        emit('GLX-FBConfig-count', len(configs))
        queue, seen = list(x.children(x.root)), set()
        while queue and len(seen) < 256:
            window = queue.pop(0)
            if window in seen:
                continue
            seen.add(window)
            a, errors = x.attributes(window)
            if a is None:
                emit('window-query-error', dict(window=hex(window), errors=errors))
                continue
            visual = a.visual.contents
            emit('window', dict(window=hex(window), pid=x.property(window, '_NET_WM_PID'),
                                wm_class=x.property(window, 'WM_CLASS'), x=a.x, y=a.y,
                                width=a.width, height=a.height, depth=a.depth, visual=hex(visual.visualid),
                                rgb_masks=[hex(visual.red_mask), hex(visual.green_mask), hex(visual.blue_mask)],
                                map_state=('Unmapped', 'Unviewable', 'Viewable')[a.map_state],
                                override_redirect=bool(a.override_redirect), window_class=a.class_))
            if a.class_ == 1:  # InputOutput; include unmapped windows to expose BadMatch.
                pixmap_trial(x, composite, glx, configs, window, a)
            queue.extend(x.children(window))
        emit('window-snapshot', dict(count=len(seen), truncated=bool(queue)))
        print('DROIDDECK_X11_DIAGNOSTICS_DONE (snapshot only; no GPU/UI success claim)', flush=True)
    finally:
        x.close()


if __name__ == '__main__':
    run()
