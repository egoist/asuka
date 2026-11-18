#!/bin/sh
# Installs the asuka CLI on macOS and Linux from its GitHub release, and
# the libraries it carries prebuilt for this computer:
#
#	curl -fsSL https://asuka.egoist.dev/install.sh | sh
#
# ASUKA_VERSION picks a release (default: the latest); ASUKA_INSTALL the
# directory (default: ~/.asuka/bin). On Windows: scripts/install.ps1.
# Once installed, asuka upgrade updates it.
set -eu

repo="egoist/asuka"
dir="${ASUKA_INSTALL:-$HOME/.asuka/bin}"

case "$(uname -s)" in
Darwin) os=darwin ;;
Linux) os=linux ;;
*)
	echo "asuka: this script installs on macOS and Linux; on Windows, in PowerShell:" >&2
	echo "  irm https://asuka.egoist.dev/install.ps1 | iex" >&2
	exit 1
	;;
esac
case "$(uname -m)" in
arm64 | aarch64) cpu=arm64 ;;
x86_64 | amd64) cpu=x64 ;;
*) echo "asuka: no build for $(uname -m)" >&2; exit 1 ;;
esac
if [ "$os" = darwin ] && [ "$cpu" = x64 ]; then
	echo "asuka: Asuka develops on Macs with Apple silicon, not Intel Macs (the apps it builds run on both: asuka build --arch amd64)" >&2
	exit 1
fi

if [ -n "${ASUKA_VERSION:-}" ]; then
	base="https://github.com/$repo/releases/download/v${ASUKA_VERSION#v}"
else
	base="https://github.com/$repo/releases/latest/download"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"
# sum_of prints SHA256SUMS' line's hash for the file $1, if it has one.
sum_of() { awk -v n="$1" '$2 == n || $2 == "*" n { print $1 }' "$tmp/SHA256SUMS"; }
# Packed, a third of its size; a release before those (0.2.9 and
# earlier) has the executable as it is.
asset="asuka-$os-$cpu.tar.gz"
want=$(sum_of "$asset")
if [ -z "$want" ]; then
	asset="asuka-$os-$cpu"
	want=$(sum_of "$asset")
fi
if [ -z "$want" ]; then
	echo "asuka: SHA256SUMS has no asuka-$os-$cpu" >&2
	exit 1
fi
echo "asuka: downloading $asset"
curl -fsSL -o "$tmp/$asset" "$base/$asset"
if command -v sha256sum >/dev/null; then got=$(sha256sum "$tmp/$asset" | cut -d' ' -f1); else got=$(shasum -a 256 "$tmp/$asset" | cut -d' ' -f1); fi
if [ "$want" != "$got" ]; then
	echo "asuka: the download doesn't match its checksum" >&2
	exit 1
fi
case "$asset" in
*.tar.gz)
	tar -xzf "$tmp/$asset" -C "$tmp" asuka
	exe="$tmp/asuka"
	;;
*) exe="$tmp/$asset" ;;
esac
mkdir -p "$dir"
install -m 0755 "$exe" "$dir/asuka"
echo "asuka: installed $dir/asuka ($("$dir/asuka" version))"
# This computer's prebuilt libraries (its own apps'), checked against the
# checksums the CLI carries, so the first build doesn't wait for them;
# if that fails, the first build fetches them.
"$dir/asuka" prebuilt fetch || echo "asuka: the prebuilt libraries weren't fetched; the first build fetches them" >&2
case ":$PATH:" in
*":$dir:"*) ;;
*) echo "asuka: add $dir to your PATH" ;;
esac
