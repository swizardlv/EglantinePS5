#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run on the BC250. Bazzite's host stays immutable; packages live in Distrobox.
set -euo pipefail
base="$HOME/eglantine-debug"
mkdir -p "$base/downloads" "$base/evidence" "$base/ghidra-projects"
if ! podman container exists eglantine-debug; then
    distrobox create --yes --name eglantine-debug --image registry.fedoraproject.org/fedora:44
fi
distrobox enter --name eglantine-debug -- sudo dnf install -y \
    gdb strace perf java-25-openjdk-devel unzip binutils elfutils procps-ng python3
archive="$base/downloads/ghidra_12.2_PUBLIC_20261016.zip"
checksum=2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67
if ! echo "$checksum  $archive" | sha256sum --check --status; then
    curl -fL --retry 3 -o "$archive" \
        https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.2_build/ghidra_12.2_PUBLIC_20261016.zip
fi
echo "$checksum  $archive" | sha256sum --check
if [[ ! -d $base/ghidra_12.2_PUBLIC ]]; then
    distrobox enter --name eglantine-debug -- unzip -q "$archive" -d "$base"
fi
distrobox enter --name eglantine-debug -- java -version
distrobox enter --name eglantine-debug -- "$base/ghidra_12.2_PUBLIC/support/analyzeHeadless" \
    "$base/ghidra-projects" smoke -import /usr/bin/true -overwrite -analysisTimeoutPerFile 60 \
    > "$base/evidence/ghidra-smoke.txt" 2>&1
gdb -batch -ex run --args /usr/bin/true > "$base/evidence/gdb-smoke.txt" 2>&1
distrobox enter --name eglantine-debug -- strace -c true > "$base/evidence/strace-smoke.txt" 2>&1
distrobox enter --name eglantine-debug -- perf stat -e task-clock -- true \
    > "$base/evidence/perf-smoke.txt" 2>&1
vulkaninfo --summary > "$base/evidence/vulkan-summary.txt" 2>&1
