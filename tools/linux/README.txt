Linux AppImage and BC250 testing (x86_64)

Build and install
  make linux-appimage JOBS=4
  make linux-install

The build copies this repository's tracked Eglantine/ tree into build/linux/source,
including any local source edits. It builds the shared core's Linux Qt/SDL frontends;
the Qt executable currently uses the upstream name lichii. The PS5 frontend, SDK,
platform allocator and bundled PS5 RADV driver are separate from this Linux target.
Existing kernel, audio, NUP and Zelda shader fixes come from the in-repo core.
This Linux target uses the core's POSIX shared backing and fastmem arena, alongside
the port's sparse-table and JIT allocators. Desktop frame metrics come from
Qt/MangoHud; the PS5 HUD stays disabled.

Requirements: Linux x86_64, CMake >=3.31, Clang, Ninja, Python >=3.11, rsync, curl,
binutils, and Qt 6 development packages. On Debian 13:
  sudo apt-get install clang cmake ninja-build python3 rsync curl binutils git \
    autoconf automake libtool nasm pkg-config glslang-tools \
    qt6-base-dev qt6-base-private-dev qt6-charts-dev qt6-tools-dev qt6-wayland qt6-svg-plugins \
    libglu1-mesa-dev libpulse-dev libudev-dev libasound2-dev libxkbcommon-dev \
    libxkbcommon-x11-0 libavfilter-dev libavcodec-dev libswscale-dev \
    libva-dev libvdpau-dev libusb-1.0-0-dev libboost-fiber-dev libopus-dev \
    zlib1g-dev libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
CC/CXX override the default clang/clang++ compilers; QMAKE overrides /usr/bin/qmake6.
For additional CMake options, invoke tools/linux/build-appimage.sh directly.

Outputs in dist/:
  EglantinePS5-Linux-x86_64.AppImage
  EglantinePS5-Linux-x86_64.debug.tar.gz
  EglantinePS5-Linux-SHA256SUMS
  EglantinePS5-Linux-build.json

The build retains frame pointers and separate DWARF symbols. Its receipt records
the repository commit and exact source-tree stamp, including local edits.
The linuxdeploy tools are downloaded and checksum checked. Their continuous-release
URLs may change: a checksum failure requires reviewing the new release and updating
the pins. Automatic emulator updates are disabled for these development builds.
The build also runs the real-wrapper 64-bit wait ABI check and checks the existing
synthetic SPEC-numbered NUP fixture against the current parser and loader APIs.
To confirm the ABI check catches the original AArch64 truncation:
  python3 tools/linux/checks/wait64_abi_check.py build/linux/source --negative-control

SSH deployment options:
  BC250_HOST=bazzite@192.168.0.167       default host/user
  BC250_APP_NAME=EglantinePS5           directory name under ~/Applications/
  BC250_SSH_CONTROL_PATH=/path/socket  optional authenticated SSH connection
No passwords or SSH keys are stored. SSH otherwise prompts normally.

Close the test app before reinstalling. Installation verifies both artifacts on the
board, extracts a fresh library tree, installs matching symbols and adds a desktop
entry. It uses its own profile/config/cache/data below ~/Applications/<APP_NAME>/.
It preserves the user's existing Eden/Lichii installation and profile.
The default launcher is ~/Applications/EglantinePS5/launch; QT_QPA_PLATFORM defaults
to xcb for Bazzite Deck and can be overridden. Import keys/firmware only for encrypted
NCA/NSP/XCI games. Unencrypted NUP/extracted dumps do not require them.

Debugging tools on Bazzite
Copy setup-debug-bc250.sh to the board and run it there:
  bash setup-debug-bc250.sh
It installs perf, strace, Java 25 and related tools in an eglantine-debug Distrobox,
then downloads/checks Ghidra 12.2 and performs small tool smoke tests. Bazzite already
provides host GDB, MangoHud and vulkaninfo. Host kernel security settings are unchanged.
Evidence is saved below ~/eglantine-debug/evidence/.
  distrobox enter --name eglantine-debug -- \
    ~/eglantine-debug/ghidra_12.2_PUBLIC/ghidraRun
  distrobox enter --name eglantine-debug -- \
    ~/eglantine-debug/ghidra_12.2_PUBLIC/support/analyzeHeadless \
    ~/eglantine-debug/ghidra-projects PROJECT -import /path/to/executable
Ghidra inspects static code. GDB inspects the host process; guest JIT frames need
additional guest-PC mapping for symbolization. MangoHud provides frame-time logging.

Execution captures
Copy capture-bc250.sh, zelda-run.py and focus-game.py together onto the board,
for example in ~/eglantine-debug/. Run there with a game dump you supply:
  ~/eglantine-debug/capture-bc250.sh run /path/game.nup
  ~/eglantine-debug/capture-bc250.sh appimage /path/game.nup
  ~/eglantine-debug/capture-bc250.sh mangohud /path/game.nup
  ~/eglantine-debug/capture-bc250.sh gdb /path/game.nup
  ~/eglantine-debug/capture-bc250.sh perf /path/game.nup
  ~/eglantine-debug/capture-bc250.sh strace /path/game.nup
CAPTURE_SECONDS bounds a normal CLI/AppImage run. EDEN_LINUX_APP_DIR selects a
nondefault installation; EDEN_LINUX_CLI selects a candidate CLI. Evidence goes into
an adjacent evidence/<UTC timestamp>-<mode>/ directory. Perf and strace execute
inside Distrobox; account for that distinction when comparing performance.

Zelda title-screen reproduction:
  python3 ~/eglantine-debug/zelda-run.py title-check \
    --game /path/game.nup --seconds 240 --gui --focus
Use the stock Switch 2 guest memory layout (12 GiB virtual address layout), as tested.
--fresh-cache selects a per-run cache; --cli /path/to/candidate/eden-cli selects a CLI
candidate instead of --gui. Each tag creates a new evidence directory.
The focus helper temporarily selects the actual X11 game window for Gamescope's
native captures, then restores the previous focus and STEAM_GAME property. Normal
desktop use needs no focus helper. The harness captures real images, logs and
MangoHud CSVs. A bounded run ending with timeout status 124 is expected; inspect the
actual images and logs to establish success. Full gameplay remains a separate test.

Rapid CLI experiments
  cmake --build build/linux/cmake --target yuzu-cmd --parallel 4
  BC250_SSH_CONTROL_PATH=/path/socket bash tools/linux/deploy-cli-candidate.sh TAG
Use a new TAG only after a successful build. The candidate receipt records its ELF
hash and source-tree stamp. The installed AppImage is preserved; candidate debug
symbols stay in build/linux/candidates/TAG/ on the build computer.
