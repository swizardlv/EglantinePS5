#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the CPU video frame payload used by both present backends."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cpu-video-frame-') as directory:
    output = Path(directory) / 'check'
    compiler = shutil.which('clang++-18') or shutil.which('clang++') or 'g++'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I', str(root / 'Eglantine/src'),
                    str(root / 'headless/cpu_video_frame_check.cpp'), '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
