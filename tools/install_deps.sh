#!/usr/bin/env bash
# Install everything needed for autonomous firmware work.
# Idempotent: safe to re-run.
#
# System packages only. Go is not needed here any more: the desktop app and
# its `core` CLI live in github.com/BrandonDedolph/core-desktop-app, which has
# its own setup. `make ipod` packs with tools/pack_ipod.py (Python 3).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

say() { printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
ok()  { printf "    \033[1;32m✓\033[0m %s\n" "$*"; }
warn(){ printf "    \033[1;33m!\033[0m %s\n" "$*"; }

# ---------- 1. system packages (distro-aware) ----------

# Detect package manager. Map our canonical needs to per-distro names.
DISTRO=""
if [[ -r /etc/os-release ]]; then
    . /etc/os-release
    DISTRO="${ID:-}"
    DISTRO_LIKE="${ID_LIKE:-}"
fi

case "$DISTRO $DISTRO_LIKE" in
    *arch*|*manjaro*|*endeavouros*)
        PM="pacman"
        PKGS=(
            # C build
            meson ninja pkgconf cmake clang
            # Sim
            sdl2
            # ARM cross-compile
            arm-none-eabi-gcc arm-none-eabi-binutils arm-none-eabi-newlib
            # Tooling
            git curl unzip jq github-cli
        )
        is_installed() { pacman -Q "$1" >/dev/null 2>&1; }
        install_pkgs() { sudo pacman -S --needed --noconfirm "$@"; }
        ;;
    *debian*|*ubuntu*)
        PM="apt"
        PKGS=(
            meson ninja-build pkg-config cmake clang-format
            libsdl2-dev
            gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
            git curl unzip jq gh
        )
        is_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }
        install_pkgs() { sudo apt-get update -qq && sudo apt-get install -y --no-install-recommends "$@"; }
        ;;
    *fedora*|*rhel*)
        PM="dnf"
        PKGS=(
            meson ninja-build pkgconf-pkg-config cmake clang-tools-extra
            SDL2-devel
            arm-none-eabi-gcc-cs arm-none-eabi-binutils-cs arm-none-eabi-newlib
            git curl unzip jq gh
        )
        is_installed() { rpm -q "$1" >/dev/null 2>&1; }
        install_pkgs() { sudo dnf install -y "$@"; }
        ;;
    *)
        echo "Unsupported distro: $DISTRO ($DISTRO_LIKE). Install these manually:" >&2
        echo "  meson ninja pkg-config cmake sdl2(dev) arm-none-eabi-{gcc,binutils,newlib} jq gh" >&2
        exit 1
        ;;
esac

MISSING=()
for p in "${PKGS[@]}"; do
    if ! is_installed "$p"; then
        MISSING+=("$p")
    fi
done

if [[ ${#MISSING[@]} -eq 0 ]]; then
    say "System packages ($PM): all present"
else
    say "Installing ${#MISSING[@]} packages via $PM"
    printf "    %s\n" "${MISSING[@]}"
    install_pkgs "${MISSING[@]}"
fi

# ---------- 2. verification ----------

say "Verifying"
ok "python3        $(python3 --version | awk '{print $2}')"
ok "meson          $(meson --version)"
ok "ninja          $(ninja --version)"
ok "arm-none-eabi  $(arm-none-eabi-gcc --version | head -1 | awk '{print $NF}')"
ok "sdl2           $(pkg-config --modversion sdl2)"
ok "gh             $(gh --version 2>/dev/null | head -1 || echo 'not installed (run: sudo apt install gh)')"

say "Ready"
echo "    Repo:    $REPO_ROOT"
echo "    ARM:     $(arm-none-eabi-gcc -dumpversion)"
echo
echo "Start me with --dangerously-skip-permissions for unattended work."
