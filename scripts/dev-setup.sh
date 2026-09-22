#!/usr/bin/env bash
# Bootstrap a TheArchive dev environment. Detects Arch or Debian/Ubuntu.
#
# Drogon itself is NOT installed here — CMake FetchContent pulls and builds it, so
# a dev machine and the deployment container resolve identical dependencies. That
# matters more than usual here: the server is rolling-release Arch, and we do not
# want the app's dependency graph moving when the host updates.
set -euo pipefail

if [[ $EUID -eq 0 ]]; then SUDO=""; else SUDO="sudo"; fi

detect_distro() {
  [[ -r /etc/os-release ]] || { echo "cannot read /etc/os-release" >&2; exit 1; }
  # shellcheck disable=SC1091
  . /etc/os-release
  case "${ID:-}${ID_LIKE:-}" in
    *arch*)            echo arch ;;
    *debian*|*ubuntu*) echo debian ;;
    *) echo "unsupported distro: ${PRETTY_NAME:-unknown}" >&2; exit 1 ;;
  esac
}

setup_arch() {
  echo "==> Synchronising packages (rolling release, so this may pull a lot)"
  $SUDO pacman -Syu --noconfirm --needed archlinux-keyring

  echo "==> Installing build toolchain, library headers, and Node"
  # Arch ships headers in the main packages; there is no -dev split.
  # libuuid comes from util-linux, which is already in base.
  $SUDO pacman -S --noconfirm --needed \
    base-devel cmake ninja git curl \
    jsoncpp openssl zlib sqlite argon2 brotli \
    libvips libheif \
    `# libheif is an *optional* dependency of libvips on Arch, so HEIC decode is` \
    `# silently absent without it — Debian's libvips42 depends on it directly.` \
    nodejs npm
}

setup_debian() {
  echo "==> Updating package lists"
  $SUDO apt-get update -qq

  echo "==> Installing build toolchain and library headers"
  $SUDO apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libsqlite3-dev \
    libargon2-dev libbrotli-dev libvips-dev sqlite3

  echo "==> Installing Node.js 22 (frontend build only)"
  if ! command -v node >/dev/null 2>&1; then
    curl -fsSL https://deb.nodesource.com/setup_22.x | $SUDO -E bash -
    $SUDO apt-get install -y nodejs
  fi
}

DISTRO="$(detect_distro)"
echo "==> Detected: $DISTRO"
"setup_$DISTRO"

echo
echo "==> Installed:"
printf '  %-8s %s\n' gcc   "$(g++ --version | head -1)"
printf '  %-8s %s\n' cmake "$(cmake --version | head -1)"
printf '  %-8s %s\n' node  "$(node --version)"
printf '  %-8s %s\n' npm   "$(npm --version)"
echo
echo "Done. The first build will take a few minutes while Drogon compiles."
