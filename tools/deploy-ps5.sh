#!/usr/bin/env bash
# deploy-ps5.sh -- one-command EglantinePS5 build + deploy + verify for the PS5.
#
# Why this script exists: deploys used to be manual (make install writes only
# the internal copy; the USB copy needed a separate FTP upload; and nobody
# checked whether the RUNNING app process had actually reloaded the new
# eboot). The result: several rounds of "testing" a build that was never
# running. This script closes the loop.
#
# Usage (run inside tmux on 253; builds take a long time):
#   ./deploy-ps5.sh build   [PS5_HOST]   Build + deploy internal + deploy USB
#                                        + verify file hashes on both copies.
#   ./deploy-ps5.sh deploy  [PS5_HOST]   Deploy the current local release files
#                                        (no rebuild) + verify file hashes.
#   ./deploy-ps5.sh verify  [PS5_HOST]   Check the RUNNING build from the latest
#                                        eden log. Must be run AFTER the user
#                                        fully quit the app / rebooted the PS5
#                                        and launched a game.
#   ./deploy-ps5.sh full    [PS5_HOST]   build, then print the restart steps.
#
# After build/deploy, the user MUST fully quit the emulator app on the PS5
# (highlight the app -> Options -> Close) or reboot the PS5, then launch a
# game, then run `./deploy-ps5.sh verify`. A file-hash match alone does NOT
# prove the running process reloaded the new binary -- only the EDEN_BUILD
# line in the log proves it.
#
# Files deployed (both copies): eboot.bin, lapy.elf, lapy-manifest.json,
# sce_sys/param.json. State is recorded in $STATE_FILE.
set -euo pipefail

REPO=/data/code/EglantinePS5
RELEASE="$REPO/build/release/PPSA98515"
CONTAINER=gallant_banach
PORT=2121
HOST="${2:-192.168.88.33}"
STATE_FILE=/home/swizard/deploy-state.json
FILES="eboot.bin lapy.elf lapy-manifest.json sce_sys/param.json"
BASES="/data/homebrew/PPSA98515 /mnt/usb1/homebrew/PPSA98515"

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "== $*"; }

# --- embedded python: FTP upload + hash verify + log fetch -----------------
# Usage: python3 - <upload|verify|fetch-log> <host> <port> <args...>
#   upload <local_dir> <file1> [file2...]   -> uploads to the USB copy
#   verify <local_dir> <file1> [file2...]   -> hashes both copies, prints JSON
#   fetch-log <remote> <local>              -> downloads one file
ftp_py() {
python3 - "$@" <<'PYEOF'
import ftplib, hashlib, json, sys, os

mode, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
BASES = ["/data/homebrew/PPSA98515", "/mnt/usb1/homebrew/PPSA98515"]

def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def connect():
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login()
    return ftp

if mode == "upload":
    local_dir = sys.argv[4]
    files = sys.argv[5:]
    ftp = connect()
    try:
        for f in files:
            with open(os.path.join(local_dir, f), "rb") as fh:
                ftp.storbinary("STOR /mnt/usb1/homebrew/PPSA98515/" + f, fh)
            print("uploaded USB:", f)
    finally:
        ftp.quit()
elif mode == "verify":
    local_dir = sys.argv[4]
    files = sys.argv[5:]
    ftp = connect()
    out = {}
    try:
        for f in files:
            local = sha_file(os.path.join(local_dir, f))
            entry = {"local": local, "copies": {}}
            for base in BASES:
                h = hashlib.sha256()
                try:
                    ftp.retrbinary("RETR " + base + "/" + f, h.update)
                    entry["copies"][base] = h.hexdigest()
                except Exception as e:
                    entry["copies"][base] = "ERR:" + str(e)
            out[f] = entry
    finally:
        ftp.quit()
    print(json.dumps(out, indent=1))
elif mode == "fetch-log":
    remote, local = sys.argv[4], sys.argv[5]
    ftp = connect()
    try:
        with open(local, "wb") as fh:
            ftp.retrbinary("RETR " + remote, fh.write)
    finally:
        ftp.quit()
    print("fetched", remote, "->", local)
else:
    sys.exit("unknown mode: " + mode)
PYEOF
}

record_state() {
    local parent eden eboot
    parent=$(cd "$REPO" && git rev-parse HEAD)
    eden=$(cd "$REPO/Eglantine" && git rev-parse HEAD)
    eboot=$(sha256sum "$RELEASE/eboot.bin" | cut -d' ' -f1)
    python3 - "$STATE_FILE" "$parent" "$eden" "$eboot" "$HOST" <<'PYEOF'
import json, sys, datetime
path, parent, eden, eboot, host = sys.argv[1:6]
state = {
    "parent_commit": parent,
    "eden_commit": eden,
    "eboot_sha256": eboot,
    "ps5_host": host,
    "deployed_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}
with open(path, "w") as fh:
    json.dump(state, fh, indent=1)
print("state recorded:", path)
print(json.dumps(state, indent=1))
PYEOF
}

do_build() {
    info "building + installing to internal copy (PS5 $HOST:$PORT)"
    info "this runs inside container $CONTAINER; keep it in tmux"
    local log="/tmp/eglantine-deploy-$(date +%Y%m%d-%H%M%S).log"
    docker exec "$CONTAINER" bash -lc \
        "cd $REPO && make install PS5_HOST=$HOST PS5_PORT=$PORT" 2>&1 | tee "$log"
    info "build log: $log"
    # make install rebuilds/repackages eboot.bin, so hashes are taken AFTER it.
    for f in $FILES; do
        [ -f "$RELEASE/$f" ] || die "missing local file after build: $RELEASE/$f"
    done
}

do_usb_upload() {
    info "uploading to USB copy (/mnt/usb1/homebrew/PPSA98515)"
    ftp_py upload "$HOST" "$PORT" "$RELEASE" $FILES
}

do_verify_files() {
    info "verifying file hashes on BOTH copies vs local post-build files"
    local out
    out=$(ftp_py verify "$HOST" "$PORT" "$RELEASE" $FILES)
    echo "$out"
    # fail loudly on any mismatch
    python3 - "$out" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
bad = []
for f, entry in data.items():
    for base, h in entry["copies"].items():
        if h != entry["local"]:
            bad.append(f"{f} on {base}: got {h[:16]} want {entry['local'][:16]}")
if bad:
    print("FILE VERIFY FAILED:")
    for b in bad:
        print("  -", b)
    sys.exit(1)
print("FILE VERIFY OK: all 4 files match on both copies.")
PYEOF
}

do_verify_running() {
    info "fetching latest eden log from PS5 ($HOST)"
    local eden_log=/tmp/deploy-verify-eden.log
    ftp_py fetch-log "$HOST" "$PORT" "/data/eglantineps5/user/log/eden_log.txt" "$eden_log"
    echo "--- session begin / build lines ---"
    grep -E "EDEN_GAME_SESSION_BEGIN|EDEN_BUILD" "$eden_log" | tail -5 || true
    echo "--- gpu41 lines (legacy marker, builds before EDEN_BUILD) ---"
    grep -E "CreateGraphicsDevice|unimplemented function '41'" "$eden_log" | tail -3 || true
    if [ ! -f "$STATE_FILE" ]; then
        echo "no state file ($STATE_FILE); cannot compare. Showing log lines only."
        return 0
    fi
    python3 - "$eden_log" "$STATE_FILE" <<'PYEOF'
import json, re, sys
log_path, state_path = sys.argv[1], sys.argv[2]
log = open(log_path, errors="replace").read()
state = json.load(open(state_path))
eden = state["eden_commit"]
m = re.findall(r"EDEN_BUILD scm_rev=(\S+)", log)
if m:
    running = m[-1]
    print(f"deployed eden commit: {eden}")
    print(f"running  eden commit: {running}")
    if running.startswith(eden[:12]) or eden.startswith(running[:12]):
        print("RUNNING = DEPLOYED. The new build is actually running.")
    else:
        print("STALE PROCESS: the running build is NOT the deployed one.")
        print("Fully quit the emulator app on the PS5 (Options -> Close)")
        print("or reboot the PS5, launch a game, then run verify again.")
    sys.exit(0)
# No EDEN_BUILD line: build predates the version log (e.g. build33).
print("No EDEN_BUILD line in log (build predates the version log).")
print(f"deployed eden commit: {eden}")
if "CreateGraphicsDevice" in log:
    print("The gpu41 HANDLER print is in the log.")
    if eden.startswith("d1e5af7497"):
        print("STALE PROCESS: deployed build33 unregistered gpu41, but the")
        print("running binary still has the handler. Reboot the PS5 and re-verify.")
    else:
        print("Handler present; deployed commit may still register it.")
else:
    print("No gpu41 handler print in log (auto-stub path).")
    print("Consistent with deployed build33 (gpu41 unregistered).")
PYEOF
}

cmd="${1:-help}"
case "$cmd" in
    build)
        do_build
        do_usb_upload
        do_verify_files
        record_state
        echo
        echo "DEPLOY DONE (files verified on both copies)."
        echo "NOW: fully quit the emulator app on the PS5 (Options -> Close)"
        echo "or reboot the PS5, launch a game, then run:"
        echo "  ./deploy-ps5.sh verify $HOST"
        ;;
    deploy)
        do_usb_upload
        do_verify_files
        record_state
        echo
        echo "DEPLOY DONE (files verified on both copies)."
        echo "NOW: fully quit the emulator app / reboot the PS5, launch a game,"
        echo "then run:  ./deploy-ps5.sh verify $HOST"
        ;;
    verify)
        do_verify_running
        ;;
    full)
        do_build
        do_usb_upload
        do_verify_files
        record_state
        echo
        echo "BUILD+DEPLOY DONE (files verified on both copies)."
        echo "NOW: fully quit the emulator app / reboot the PS5, launch a game,"
        echo "then run:  ./deploy-ps5.sh verify $HOST"
        ;;
    *)
        echo "usage: $0 {build|deploy|verify|full} [PS5_HOST]"
        echo "  default PS5_HOST is 192.168.88.33"
        exit 1
        ;;
esac
