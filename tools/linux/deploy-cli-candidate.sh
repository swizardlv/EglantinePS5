#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Deploy a built CLI for an isolated experiment; the installed AppImage stays intact.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
tag=${1:?usage: deploy-cli-candidate.sh TAG}
[[ $tag =~ ^[a-z0-9][a-z0-9-]*$ ]] || { echo 'Invalid candidate tag' >&2; exit 2; }
work="$root/build/linux/candidates/$tag"
[[ ! -e $work ]] || { echo "Candidate already exists: $tag" >&2; exit 2; }
mkdir -p "$work"
source_bin="$root/build/linux/cmake/bin/eden-cli"
readelf -h "$source_bin" > /dev/null
objcopy --only-keep-debug "$source_bin" "$work/eden-cli.debug"
strip --strip-debug -o "$work/eden-cli" "$source_bin"
objcopy --add-gnu-debuglink="$work/eden-cli.debug" "$work/eden-cli"
chmod +x "$work/eden-cli"
python3 - "$root" "$work" "$tag" <<'PY'
import hashlib,json,pathlib,subprocess,sys
root,work=map(pathlib.Path,sys.argv[1:3])
receipt={'tag':sys.argv[3], 'port_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(), 'source_stamp':(root/'build/linux/source/.unpacked-from').read_text().strip(), 'cli_sha256':hashlib.file_digest((work/'eden-cli').open('rb'),'sha256').hexdigest()}
(work/'candidate.json').write_text(json.dumps(receipt,indent=2)+'\n')
PY
(cd "$work" && sha256sum eden-cli > SHA256SUMS)
host=${BC250_HOST:-bazzite@192.168.0.167}
ssh_args=()
[[ -z ${BC250_SSH_CONTROL_PATH:-} ]] || ssh_args+=(-o "ControlPath=$BC250_SSH_CONTROL_PATH")
remote="Applications/EglantinePS5/candidates/$tag"
ssh "${ssh_args[@]}" "$host" "mkdir -p $remote"
scp "${ssh_args[@]}" "$work/eden-cli" "$work/candidate.json" "$work/SHA256SUMS" "$host:$remote/"
ssh "${ssh_args[@]}" "$host" "cd $remote && sha256sum --check SHA256SUMS"
echo "Remote candidate: ~/Applications/EglantinePS5/candidates/$tag/eden-cli"
