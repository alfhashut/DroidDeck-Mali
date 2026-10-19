"""Check error attribution/resource cleanup without a GPU or X server."""
import contextlib
import ctypes as C
import io
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

import diagnose_steam_x11 as diag


class PixmapDiagnosticsTests(unittest.TestCase):
    def fixture(self, name_error=False, glx_error=False):
        functions = SimpleNamespace(XFreePixmap=Mock())
        composite = SimpleNamespace(XCompositeNameWindowPixmap=Mock())
        glx = SimpleNamespace(glXCreatePixmap=Mock(), glXDestroyPixmap=Mock())
        x = SimpleNamespace(d=1, x=functions, geometry=Mock(return_value={'depth': 32}))

        def checked(fn, *args):
            fn(*args)
            if fn is composite.XCompositeNameWindowPixmap:
                return 123, [{'error': 'BadMatch', 'opcode': 142, 'minor': 6}] if name_error else []
            if fn is glx.glXCreatePixmap:
                return 456, [{'error': 'BadDrawable'}] if glx_error else []
            return 0, []

        x.checked = checked
        visual = diag.Visual(visualid=532)
        attr = diag.Attributes(depth=32, visual=C.pointer(visual))
        configs = [(7, dict(visual=532, depth=32, drawable_type=7, id=435))]
        return x, composite, glx, configs, attr

    def test_failed_composite_xid_never_reaches_glx_or_free(self):
        x, composite, glx, configs, attr = self.fixture(name_error=True)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            diag.pixmap_trial(x, composite, glx, configs, 99, attr)
        self.assertIn('BadMatch', output.getvalue())
        glx.glXCreatePixmap.assert_not_called()
        x.x.XFreePixmap.assert_not_called()

    def test_successful_refs_are_released_without_context_or_rendering(self):
        x, composite, glx, configs, attr = self.fixture()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            diag.pixmap_trial(x, composite, glx, configs, 99, attr)
        glx.glXDestroyPixmap.assert_called_once_with(1, 456)
        x.x.XFreePixmap.assert_called_once_with(1, 123)
        self.assertIn('Steam FBConfig unknown', output.getvalue())

    def test_glx_failure_only_frees_valid_composite_reference(self):
        x, composite, glx, configs, attr = self.fixture(glx_error=True)
        with contextlib.redirect_stdout(io.StringIO()):
            diag.pixmap_trial(x, composite, glx, configs, 99, attr)
        glx.glXDestroyPixmap.assert_not_called()
        x.x.XFreePixmap.assert_called_once_with(1, 123)

    def test_different_visual_does_not_claim_fbconfig_compatibility(self):
        x, composite, glx, configs, attr = self.fixture()
        configs[0][1]['visual'] = 35
        with contextlib.redirect_stdout(io.StringIO()) as output:
            diag.pixmap_trial(x, composite, glx, configs, 99, attr)
        glx.glXCreatePixmap.assert_not_called()
        self.assertIn('matching-GLX-pixmap-FBConfigs=[]', output.getvalue())
        x.x.XFreePixmap.assert_called_once()


if __name__ == '__main__':
    unittest.main()
