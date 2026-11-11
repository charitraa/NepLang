#!/bin/sh
# Runs upload.sh inside an Ubuntu container, so the Debian tools (debuild, dput) don't
# have to be installed on this machine. Your GPG secret key is copied into the throwaway
# container for signing (it stays passphrase-protected) and deleted afterwards.
#
#   packaging/ppa/upload-docker.sh VERSION [KEYID] ["SERIES ..."]
#
# KEYID defaults to the key whose user ID matches DEBEMAIL (the maintainer address).
set -eu

[ $# -ge 1 ] || { echo "usage: $0 VERSION [KEYID] [\"SERIES ...\"]" >&2; exit 1; }
version=$1
email="${DEBEMAIL:-stharabi9862187405@gmail.com}"
key=${2:-$email}
series=${3:-}

root=$(git rev-parse --show-toplevel)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
chmod 700 "$tmp"
gpg --export-secret-keys --armor "$key" > "$tmp/key.asc"
[ -s "$tmp/key.asc" ] || { echo "no GPG secret key found for '$key'" >&2; exit 1; }

docker run -it --rm \
    -v "$root:/src:ro" -v "$tmp:/keys:ro" \
    -e DEBEMAIL="$email" -e PPA="${PPA:-}" \
    ubuntu:24.04 bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq devscripts debhelper dput gnupg git >/dev/null 2>&1
export GPG_TTY=$(tty)
gpg --batch --import /keys/key.asc
git config --global --add safe.directory "*"
git clone -q /src /work && cd /work
[ -n "$PPA" ] || unset PPA
packaging/ppa/upload.sh '"$version"' '"${series:+\"$series\"}"'
'
