#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The diagnostic movie replacement must never hide the guest UI by default."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
text = (root/'Eglantine/src/core/hle/service/nx2_gsv.h').read_text()
start = text.index('inline bool Nx2VideoReplacementEnabled()')
end = text.index('\n}', start) + 2
body = text[start:end]
with tempfile.TemporaryDirectory(prefix='video-replacement-') as directory:
    cpp = Path(directory)/'test.cpp'
    exe = Path(directory)/'test'
    cpp.write_text('#include <cstdlib>\n' + body + '''
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const bool expected = argv[1][0] == '1';
    if (Nx2VideoReplacementEnabled() != expected) return 1;
    setenv("EDEN_NX2_VIDEO_OVERLAY", expected ? "0" : "1", 1);
    return Nx2VideoReplacementEnabled() == expected ? 0 : 1;
}
''')
    compiler = shutil.which('clang++-18') or shutil.which('clang++') or 'g++'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    for value, expected in [(None, True), ('0', False), ('1', True), ('false', False)]:
        env = dict(os.environ)
        env.pop('EDEN_NX2_VIDEO_OVERLAY', None)
        if value is not None: env['EDEN_NX2_VIDEO_OVERLAY'] = value
        subprocess.run([str(exe), str(int(expected))], env=env, check=True)
print('Video replacement PASS: enabled by default, explicit opt-out, startup-only setting')
