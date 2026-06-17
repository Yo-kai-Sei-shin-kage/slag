#!/usr/bin/env bash
# install.sh — Slag toolchain installer
#
# Detects the host environment (Cygwin or a standard Linux distro),
# checks for and offers to install required dependencies, builds the
# Slag compiler, and sets up the man page so `man slag` works from any
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

# -----------------------------------------------------------------------
# Environment detection
# -----------------------------------------------------------------------

OS_KIND="unknown"
PKG_MANAGER="none"

if [ -n "$CYGWIN" ] || uname -s | grep -qi "cygwin"; then
    OS_KIND="cygwin"
elif [ -f /etc/os-release ]; then
    OS_KIND="linux"
    . /etc/os-release
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
fi

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
  - gcc-core, gcc-g++   (Devel category)
  - mingw64-x86_64-gcc-core  (Devel category — provides x86_64-w64-mingw32-gcc)
  - nasm                (Devel category)
  - git                 (Devel category)

After installing, re-run this script to verify and continue.
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
            read -r -p "Run the suggested install command now with sudo? [y/N] " REPLY
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
gcc -Wall -Wextra -o slag main.c lexer.c ast.c parser.c codegen.c window_runtime.c
echo "Built: $COMPILER_DIR/slag"
echo ""

# -----------------------------------------------------------------------
# Man page setup
# -----------------------------------------------------------------------

if [ -f "$DOC_DIR/slag.1" ]; then
    mkdir -p "$MAN1_DIR"
    cp "$DOC_DIR/slag.1" "$MAN1_DIR/slag.1"
    echo "Installed man page at $MAN1_DIR/slag.1"

    RC_FILE="$HOME/.bashrc"
    case "$SHELL" in
        *zsh)  RC_FILE="$HOME/.zshrc" ;;
        *bash) RC_FILE="$HOME/.bashrc" ;;
    esac

    MANPATH_LINE="export MANPATH=\"$DOC_DIR:\$MANPATH\""

    if grep -qF "$MANPATH_LINE" "$RC_FILE" 2>/dev/null; then
        echo "MANPATH entry already present in $RC_FILE"
    else
        echo "$MANPATH_LINE" >> "$RC_FILE"
        echo "Added MANPATH entry to $RC_FILE"
    fi
else
    echo "No man page found at $DOC_DIR/slag.1 — skipping man page setup."
fi

# -----------------------------------------------------------------------
# PATH setup for the slag binary itself
# -----------------------------------------------------------------------

RC_FILE="$HOME/.bashrc"
case "$SHELL" in
    *zsh)  RC_FILE="$HOME/.zshrc" ;;
    *bash) RC_FILE="$HOME/.bashrc" ;;
esac

PATH_LINE="export PATH=\"$COMPILER_DIR:\$PATH\""
if grep -qF "$PATH_LINE" "$RC_FILE" 2>/dev/null; then
    echo "PATH entry already present in $RC_FILE"
else
    echo "$PATH_LINE" >> "$RC_FILE"
    echo "Added PATH entry to $RC_FILE"
fi

echo ""
echo "Installation complete."
echo "Run 'source $RC_FILE' (or open a new terminal) to pick up PATH/MANPATH changes."
echo "Then: 'slag yourprogram.slag' to compile, and 'man slag' for documentation."
