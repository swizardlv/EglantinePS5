#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression checks for BC250 input validation and process/focus cleanup."""

import argparse
from contextlib import redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import shlex
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

run = load('bc250_run', 'zelda-run.py')
focus = load('bc250_focus', 'focus-game.py')

class Inputs(unittest.TestCase):
    def test_explicit_game_needs_no_private_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'game with spaces.nup'
            game.write_bytes(b'not executed')
            self.assertEqual(run.resolve_game(game, root / 'missing'), game)

    def test_missing_and_invalid_receipts_are_clear(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, 'specify --game'):
                run.resolve_game(None, root)
            (root / 'zelda-download.json').write_text('not json')
            with self.assertRaisesRegex(ValueError, 'cannot read game path'):
                run.resolve_game(None, root)

    def test_safe_tags_and_bounded_duration(self):
        for tag in ('../outside', '/absolute', 'with space', '.', 'a' * 65):
            with self.subTest(tag=tag), self.assertRaises(argparse.ArgumentTypeError):
                run.valid_tag(tag)
        self.assertEqual(run.valid_tag('CHECK_name-1'), 'CHECK_name-1')
        for duration in ('0', '9', '3601', '-1'):
            with self.subTest(duration=duration), self.assertRaises(argparse.ArgumentTypeError):
                run.capture_duration(duration)
        self.assertEqual(run.capture_duration('240'), 240)

class Properties:
    def __init__(self, game=(42,)):
        self.values = {(':0', focus.BASE_WINDOW): (0,), (':1', 'STEAM_GAME'): game,
                       (':0', 'GAMESCOPECTRL_BASELAYER_APPID'): (413091, 769),
                       (':0', 'GAMESCOPE_FOCUSED_WINDOW'): (123,),
                       (':0', 'GAMESCOPE_FOCUSED_APP'): (focus.TEST_APP_ID,)}
        self.fail_root_write = False
    def xprop(self, display, *args):
        if '-set' in args:
            at = args.index('-set')
            atom, value = args[at + 1: at + 3]
            if display == ':0' and atom == focus.BASE_WINDOW and self.fail_root_write:
                self.fail_root_write = False
                raise RuntimeError('simulated write failure')
            self.values[display, atom] = tuple(int(part.strip()) for part in value.split(','))
            return ''
        if '-remove' in args:
            self.values[display, args[-1]] = None
            return ''
        atom = args[-1]
        value = self.values.get((display, atom))
        return f'{atom}: not found.' if value is None else f'{atom}(CARDINAL) = ' + ', '.join(map(str, value))

class RestoreFocus(unittest.TestCase):
    def selection(self, directory, properties):
        return focus.FocusSelection(1234, 123, Path(directory))

    def check_restoration(self, original):
        properties = Properties(original)
        with tempfile.TemporaryDirectory() as directory, patch.object(focus, 'xprop', properties.xprop), \
                patch.object(focus, 'process_identity', return_value='start'), patch.object(focus.time, 'sleep'):
            selection = self.selection(directory, properties)
            selection.select()
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], (focus.TEST_APP_ID,))
            selection.restore()
            self.assertEqual(properties.values[':0', focus.BASE_WINDOW], (0,))
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], original)

    def test_restores_existing_steam_game(self):
        self.check_restoration((777,))

    def test_removes_only_property_that_was_originally_absent(self):
        self.check_restoration(None)

    def test_preserves_new_user_selection(self):
        properties = Properties((777,))
        with tempfile.TemporaryDirectory() as directory, patch.object(focus, 'xprop', properties.xprop), \
                patch.object(focus, 'process_identity', return_value='start'), patch.object(focus.time, 'sleep'):
            selection = self.selection(directory, properties)
            selection.select()
            properties.values[':0', focus.BASE_WINDOW] = (999,)
            properties.values[':1', 'STEAM_GAME'] = (888,)
            selection.restore()
            self.assertEqual(properties.values[':0', focus.BASE_WINDOW], (999,))
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], (888,))

    def test_partial_selection_failure_still_restores_window_property(self):
        properties = Properties((777,))
        properties.fail_root_write = True
        with tempfile.TemporaryDirectory() as directory, patch.object(focus, 'xprop', properties.xprop), \
                patch.object(focus, 'process_identity', return_value='start'):
            selection = self.selection(directory, properties)
            with self.assertRaisesRegex(RuntimeError, 'write failure'):
                selection.select()
            selection.restore()
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], (777,))

    def test_screenshot_failure_restores_main_state(self):
        properties = Properties((777,))
        with tempfile.TemporaryDirectory() as directory, patch.object(focus, '__file__', str(Path(directory)/'focus-game.py')), \
                patch.object(focus, 'xprop', properties.xprop), patch.object(focus, 'process_identity', return_value='start'), \
                patch.object(focus.time, 'sleep'), patch.object(focus.subprocess, 'run', side_effect=subprocess.TimeoutExpired('gamescopectl', 10)), \
                redirect_stderr(io.StringIO()):
            self.assertEqual(focus.main(['1234', '123', 'screenshot-error']), 1)
            self.assertEqual(properties.values[':0', focus.BASE_WINDOW], (0,))
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], (777,))

    def test_signal_restores_main_state(self):
        properties = Properties((777,))
        with tempfile.TemporaryDirectory() as directory, patch.object(focus, '__file__', str(Path(directory)/'focus-game.py')), \
                patch.object(focus, 'xprop', properties.xprop), patch.object(focus, 'process_identity', return_value='start'), \
                patch.object(focus.time, 'sleep'), patch.object(focus.subprocess, 'run', side_effect=lambda *a, **k: focus.stop(signal.SIGTERM, None)):
            with self.assertRaises(SystemExit) as stopped:
                focus.main(['1234', '123', 'signal'])
            self.assertEqual(stopped.exception.code, 0)
            self.assertEqual(properties.values[':0', focus.BASE_WINDOW], (0,))
            self.assertEqual(properties.values[':1', 'STEAM_GAME'], (777,))

class CaptureInterruption(unittest.TestCase):
    def test_unexpected_capture_exit_reaches_caller_and_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'game.nup'
            game.write_bytes(b'not executed')
            script = root / 'capture-bc250.sh'
            script.write_text('#!/usr/bin/env bash\nexit 139\n')
            script.chmod(0o755)
            with patch.object(run, '__file__', str(root/'zelda-run.py')), \
                    patch.dict(os.environ, {'EDEN_LINUX_APP_DIR': str(root/'app')}):
                status = run.main(['crashed', '--game', str(game), '--seconds', '10'])
            self.assertEqual(status, 139)
            receipt = json.loads((root/'evidence/crashed/run.json').read_text())
            self.assertEqual(receipt['exit_status'], 139)
            self.assertIsNone(receipt['interrupted_signal'])
            self.assertIsNone(receipt['error'])

    def test_sigterm_stops_private_game_group_and_writes_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'game.nup'
            game.write_bytes(b'not executed')
            script = root / 'capture-bc250.sh'
            script.write_text('#!/usr/bin/env bash\nsleep 30 &\necho $! > '+shlex.quote(str(root/'child.pid'))+'\nwait\n')
            script.chmod(0o755)
            old_sigterm = signal.getsignal(signal.SIGTERM)
            timer = threading.Timer(0.5, lambda: os.kill(os.getpid(), signal.SIGTERM))
            with patch.object(run, '__file__', str(root/'zelda-run.py')):
                timer.start()
                try:
                    status = run.main(['interrupted', '--game', str(game), '--seconds', '10'])
                finally:
                    timer.cancel(); timer.join()
            self.assertEqual(status, 128 + signal.SIGTERM)
            receipt = json.loads((root/'evidence/interrupted/run.json').read_text())
            self.assertEqual(receipt['interrupted_signal'], signal.SIGTERM)
            self.assertEqual(signal.getsignal(signal.SIGTERM), old_sigterm)
            child = int((root/'child.pid').read_text())
            stat = Path('/proc') / str(child) / 'stat'
            self.assertTrue(not stat.exists() or stat.read_text().split(') ', 1)[1].startswith('Z'))

if __name__ == '__main__':
    unittest.main(verbosity=2)
