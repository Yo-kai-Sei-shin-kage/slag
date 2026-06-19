#!/usr/bin/env bash
# install.sh — Slag toolchain installer
#
# Detects the host environment (Cygwin, MSYS2/MinGW, or a standard Linux
# distro), checks for and offers to install required dependencies, builds
# the Slag compiler, and sets up the man page so `man slag` works from any
# directory.
#
# Usage:
#   ./install.sh
#
# Required toolchain:
#   - gcc                      (native, to build the slag compiler itself)
#   - x86_64-w64-mingw32-gcc   (MinGW-w64 cross compiler/linker, used to
#                                link the Win64 executables Slag programs
#                                produce)
#   - nasm                     (assembler for the generated .asm output)
#   - git                      (only checked; not required to run this
#                                script, but flagged if missing)

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER_DIR="$REPO_DIR/compiler"
DOC_DIR="$REPO_DIR/documentation"
MAN1_DIR="$DOC_DIR/man1"

# Resolve the shell rc file once, up front (used for PATH and MANPATH).
RC_FILE="$HOME/.bashrc"
case "$SHELL" in
    *zsh)  RC_FILE="$HOME/.zshrc" ;;
    *bash) RC_FILE="$HOME/.bashrc" ;;
esac

# -----------------------------------------------------------------------
# Environment detection
# -----------------------------------------------------------------------

OS_KIND="unknown"
PKG_MANAGER="none"

UNAME_S="$(uname -s 2>/dev/null || echo unknown)"

case "$UNAME_S" in
    CYGWIN*)            OS_KIND="cygwin" ;;
    MINGW*|MSYS*)       OS_KIND="msys" ;;
    Linux*)
        OS_KIND="linux"
        if [ -f /etc/os-release ]; then
            . /etc/os-release
        fi
        if command -v apt-get >/dev/null 2>&1; then
            PKG_MANAGER="apt"
        elif command -v dnf >/dev/null 2>&1; then
            PKG_MANAGER="dnf"
        elif command -v yum >/dev/null 2>&1; then
            PKG_MANAGER="yum"
        elif command -v pacman >/dev/null 2>&1; then
            PKG_MANAGER="pacman"
        elif command -v zypper >/dev/null 2>&1; then
            PKG_MANAGER="zypper"
        fi
        ;;
esac

echo "Detected environment: $OS_KIND"
if [ "$OS_KIND" = "linux" ]; then
    echo "Detected package manager: $PKG_MANAGER"
fi
echo ""

# -----------------------------------------------------------------------
# Dependency checks
# -----------------------------------------------------------------------

MISSING=()

check_dep() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING+=("$cmd")
        echo "  [missing] $cmd"
    else
        echo "  [ok]      $cmd"
    fi
}

echo "Checking dependencies..."
check_dep gcc
check_dep nasm
check_dep git
check_dep x86_64-w64-mingw32-gcc

echo ""

if [ ${#MISSING[@]} -eq 0 ]; then
    echo "All dependencies present."
else
    echo "Missing: ${MISSING[*]}"
    echo ""

    case "$OS_KIND" in
        cygwin)
            cat <<'EOF'
This appears to be Cygwin. Cygwin packages must be installed through
the Cygwin setup program (setup-x86_64.exe), not from this script.

Re-run Cygwin's setup and ensure the following packages are selected:
  - gcc-core, gcc-g++        (Devel category)
  - mingw64-x86_64-gcc-core  (Devel category — provides x86_64-w64-mingw32-gcc)
  - nasm                     (Devel category)
  - git                      (Devel category)

After installing, re-run this script to verify and continue.
EOF
            exit 1
            ;;
        msys)
            cat <<'EOF'
This appears to be MSYS2 / MinGW. Install the required packages with
pacman, then re-run this script:

  pacman -S --needed gcc nasm git mingw-w64-x86_64-gcc

(Run from the MSYS2 shell. The mingw-w64-x86_64-gcc package provides
the cross toolchain used to link Win64 executables.)
EOF
            exit 1
            ;;
        linux)
            case "$PKG_MANAGER" in
                apt)
                    echo "Suggested install command (Debian/Ubuntu):"
                    echo "  sudo apt-get update && sudo apt-get install -y gcc nasm git mingw-w64"
                    ;;
                dnf)
                    echo "Suggested install command (Fedora):"
                    echo "  sudo dnf install -y gcc nasm git mingw64-gcc"
                    ;;
                yum)
                    echo "Suggested install command (RHEL/CentOS):"
                    echo "  sudo yum install -y gcc nasm git mingw64-gcc"
                    ;;
                pacman)
                    echo "Suggested install command (Arch):"
                    echo "  sudo pacman -S --needed gcc nasm git mingw-w64-gcc"
                    ;;
                zypper)
                    echo "Suggested install command (openSUSE):"
                    echo "  sudo zypper install -y gcc nasm git mingw64-cross-gcc"
                    ;;
                *)
                    echo "Could not detect a known package manager. Install the following"
                    echo "manually using your distribution's package manager: gcc nasm git"
                    echo "and a MinGW-w64 cross compiler (x86_64-w64-mingw32-gcc)."
                    ;;
            esac
            echo ""
            read -r -p "Run the suggested install command now with sudo? [y/N] " REPLY || REPLY=""
            if [[ "$REPLY" =~ ^[Yy]$ ]]; then
                case "$PKG_MANAGER" in
                    apt)    sudo apt-get update && sudo apt-get install -y gcc nasm git mingw-w64 ;;
                    dnf)    sudo dnf install -y gcc nasm git mingw64-gcc ;;
                    yum)    sudo yum install -y gcc nasm git mingw64-gcc ;;
                    pacman) sudo pacman -S --needed gcc nasm git mingw-w64-gcc ;;
                    zypper) sudo zypper install -y gcc nasm git mingw64-cross-gcc ;;
                    *) echo "No automatic install available; please install manually." ;;
                esac
            else
                echo "Skipping automatic install. Re-run this script after installing dependencies."
                exit 1
            fi
            ;;
        *)
            echo "Unrecognized environment. Please install manually: gcc, nasm, git,"
            echo "and a MinGW-w64 cross compiler (x86_64-w64-mingw32-gcc)."
            exit 1
            ;;
    esac

    echo ""
    echo "Re-checking dependencies after install attempt..."
    MISSING=()
    check_dep gcc
    check_dep nasm
    check_dep git
    check_dep x86_64-w64-mingw32-gcc
    echo ""

    if [ ${#MISSING[@]} -ne 0 ]; then
        echo "Still missing: ${MISSING[*]}. Please install these manually, then re-run this script."
        exit 1
    fi
fi

# -----------------------------------------------------------------------
# Build the compiler
# -----------------------------------------------------------------------

echo "Building the Slag compiler..."
cd "$COMPILER_DIR"
gcc -Wall -Wextra -o slag main.c lexer.c ast.c parser.c codegen.c \
    window_runtime.c net_runtime.c mem_runtime.c

# Decide the expected binary name per platform. On Cygwin/MSYS, GCC always
# emits a .exe regardless of the -o name; on Linux it is extensionless.
case "$OS_KIND" in
    cygwin|msys) SLAG_BIN="slag.exe" ;;
    *)           SLAG_BIN="slag" ;;
esac

if [ ! -f "$SLAG_BIN" ]; then
    echo "Build error: expected compiler binary '$SLAG_BIN' was not produced."
    echo "Check the gcc output above for errors."
    exit 1
fi

chmod +x "$SLAG_BIN"
echo "Built: $COMPILER_DIR/$SLAG_BIN"
echo ""

# -----------------------------------------------------------------------
# Man page setup
# -----------------------------------------------------------------------

if [ -f "$MAN1_DIR/slag.1" ]; then
    echo "Found man page at $MAN1_DIR/slag.1"

    # Ensure man can read the page and traverse the directories leading to
    # it. Files copied or checked out can land without world/owner read bits
    # (e.g. via odd Cygwin ACLs), which makes `man slag` fail with
    # "Permission denied" even when the path resolves correctly.
    chmod 644 "$MAN1_DIR/slag.1" 2>/dev/null || true
    chmod 755 "$DOC_DIR" "$MAN1_DIR" 2>/dev/null || true

    MANPATH_LINE="export MANPATH=\"$DOC_DIR:\$MANPATH\""

    if grep -qF "$MANPATH_LINE" "$RC_FILE" 2>/dev/null; then
        echo "MANPATH entry already present in $RC_FILE"
    else
        echo "$MANPATH_LINE" >> "$RC_FILE"
        echo "Added MANPATH entry to $RC_FILE"
    fi
else
    echo "No man page found at $MAN1_DIR/slag.1 — skipping man page setup."
fi

# -----------------------------------------------------------------------
# PATH setup for the slag binary itself
# -----------------------------------------------------------------------

PATH_LINE="export PATH=\"$COMPILER_DIR:\$PATH\""
if grep -qF "$PATH_LINE" "$RC_FILE" 2>/dev/null; then
    echo "PATH entry already present in $RC_FILE"
else
    echo "$PATH_LINE" >> "$RC_FILE"
    echo "Added PATH entry to $RC_FILE"
fi

# -----------------------------------------------------------------------
# Generate the slagrun helper (compile + assemble + link + run in one step)
# -----------------------------------------------------------------------
#
# This is the canonical build pipeline for Slag PROGRAMS (not the compiler).
# Keeping it generated here means the link flags — including -lws2_32 for the
# networking runtime — stay correct and version-controlled with the repo.

SLAGRUN="$COMPILER_DIR/slagrun"
cat > "$SLAGRUN" <<'SLAGRUN_EOF'
#!/usr/bin/env bash
# slagrun — compile, assemble, link, and run a Slag program.
# Usage: slagrun program[.slag]
set -e
base="${1%.slag}"
slag "$base.slag"
nasm -f win64 "$base.asm" -o "$base.obj"
gcc "$base.obj" -o "$base.exe" \
    -nostartfiles -e _start \
    -lkernel32 -luser32 -lgdi32 -lws2_32
"./$base.exe"
SLAGRUN_EOF
chmod +x "$SLAGRUN"
echo "Installed slagrun helper: $SLAGRUN"
echo ""

echo ""
echo "Installation complete."
echo "Run 'source $RC_FILE' (or open a new terminal) to pick up PATH/MANPATH changes."
echo "Then: 'slagrun yourprogram' to build+run, 'slag yourprogram.slag' to compile only,"
echo "and 'man slag' for documentation."
