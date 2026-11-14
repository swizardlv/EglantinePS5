#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the PS5 profile applet against host-built common libraries and real account storage."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compile-flags', type=Path, required=True)
parser.add_argument('--libraries', type=Path, required=True)
parser.add_argument('--link-directory', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
flags = json.loads(args.compile_flags.read_text()) + ['-UNDEBUG', '-I' + str(root),
                                                     '-I' + str(root / 'headless')]
libraries = json.loads(args.libraries.read_text())
with tempfile.TemporaryDirectory(prefix='eden-profile-check-') as directory:
    objects = []
    for source in [root / 'headless/profile_applet_check.cpp',
                   root / 'Eglantine/src/core/hle/service/acc/profile_manager.cpp']:
        obj = Path(directory) / (source.stem + '.o')
        subprocess.run([*flags, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    executable = Path(directory) / 'profile-applet-check'
    subprocess.run([flags[0], '-pthread', *objects, '-o', str(executable),
                    '-Wl,--start-group', *libraries, '-Wl,--end-group'],
                   cwd=args.link_directory, check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
