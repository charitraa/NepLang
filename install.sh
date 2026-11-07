#!/bin/sh
# NepLang installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/charitraa/NepLang/main/install.sh | sh
#
# Downloads the prebuilt `neplang` binary from the latest GitHub Release.
# Set NEPLANG_INSTALL_DIR to choose where it goes (default: ~/.local/bin).
# Set NEPLANG_VERSION (e.g. v0.5.0) to install a specific release.
set -eu

REPO="charitraa/NepLang"
INSTALL_DIR="${NEPLANG_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${NEPLANG_VERSION:-latest}"

fail() {
    echo "neplang install: $1" >&2
    exit 1
}

os=$(uname -s)
arch=$(uname -m)

case "$os" in
    Linux)
        case "$arch" in
            x86_64 | amd64) asset="neplang-linux-x86_64" ;;
            aarch64 | arm64) asset="neplang-linux-arm64" ;;
            *) fail "no prebuilt binary for Linux $arch. Build from source instead (see README)." ;;
        esac
        ;;
    Darwin)
        asset="neplang-macos-universal"
        ;;
    *)
        fail "unsupported OS '$os'. On Windows, use install.ps1 (see README)."
        ;;
esac

if [ "$VERSION" = "latest" ]; then
    url="https://github.com/$REPO/releases/latest/download/$asset"
else
    url="https://github.com/$REPO/releases/download/$VERSION/$asset"
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

echo "Downloading $asset ..."
if command -v curl >/dev/null 2>&1; then
    curl -fSL --progress-bar "$url" -o "$tmp" || fail "download failed: $url"
elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress "$url" -O "$tmp" || fail "download failed: $url"
else
    fail "need curl or wget to download."
fi

mkdir -p "$INSTALL_DIR"
chmod +x "$tmp"
mv "$tmp" "$INSTALL_DIR/neplang"
trap - EXIT

echo "Installed neplang to $INSTALL_DIR/neplang"

case ":$PATH:" in
    *":$INSTALL_DIR:"*)
        echo "Run it with:  neplang yourfile.np"
        ;;
    *)
        echo ""
        echo "$INSTALL_DIR is not on your PATH. Add this line to your ~/.bashrc or ~/.zshrc:"
        echo ""
        echo "    export PATH=\"$INSTALL_DIR:\$PATH\""
        echo ""
        echo "then open a new terminal and run:  neplang yourfile.np"
        ;;
esac
