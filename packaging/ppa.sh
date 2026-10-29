#!/bin/sh
# Build Ubuntu source packages of a release tag for the PPA, one per series,
# sign them with the host's gpg and, with --upload, send them to
# Launchpad. The Debian tooling runs in an ubuntu container.
#
#   packaging/ppa.sh 0.1.0                 # build and sign into build/ppa
#   packaging/ppa.sh 0.1.0 --upload        # ...and dput them
#   SERIES="noble" packaging/ppa.sh 0.1.0  # only some series
#   PPA_REV=2 OUT=build/ppa2 packaging/ppa.sh 0.1.0 --upload
#                                          # the same version again, e.g. for a
#                                          # newly enabled architecture
#
# Needs docker, git, curl, and the signing key in the host's gpg-agent.
set -eu

version=${1:?usage: ppa.sh VERSION [--upload]}
upload=${2:-}
# noble is the oldest series with CMake 3.28
series=${SERIES:-"noble resolute stonking"}
key=${DEBSIGN_KEYID:-7746D1F8D7DA0259!}
ppa=${PPA:-ppa:mattmoyles/jotdo}
maintainer="Matt Moyles <matt@moyles.xyz>"
name=jotdo
root=$(git rev-parse --show-toplevel)
out=${OUT:-$root/build/ppa}
ppa_rev=${PPA_REV:-1}
# Launchpad already has the orig tarballs after the first upload of a version
[ "$ppa_rev" -gt 1 ] && orig_opt=-sd || orig_opt=-sa
image=ubuntu:24.04

mkdir -p "$out"
"$root/packaging/debsrc.sh" "$version" "$out/src"
# dput looks for the orig tarballs next to the .changes files
cp "$out/src/${name}_$version".orig*.tar.gz "$out/"

for s in $series; do
    rev="$version-1~${s}${ppa_rev}"
    work="$out/work-$s"
    rm -rf "$work"
    mkdir -p "$work"
    cp "$out/src/${name}_$version".orig*.tar.gz "$work/"
    cp -r "$out/src/$name-$version" "$work/"
    {
        printf '%s (%s) %s; urgency=medium\n\n  * Release %s for %s.\n\n -- %s  %s\n\n' \
            "$name" "$rev" "$s" "$version" "$s" "$maintainer" "$(date -R)"
        cat "$root/packaging/debian/changelog"
    } > "$work/$name-$version/debian/changelog"
    docker run --rm -v "$work:/b" -w "/b/$name-$version" -e UID_GID="$(id -u):$(id -g)" -e ORIG_OPT="$orig_opt" "$image" sh -c '
        apt-get update -qq >/dev/null
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq dpkg-dev debhelper >/dev/null 2>&1
        dpkg-buildpackage -S "$ORIG_OPT" -d -us -uc >/b/build.log 2>&1 || { tail -20 /b/build.log; exit 1; }
        chown -R "$UID_GID" /b'
    mv "$work"/"${name}_$rev"* "$out/"
    echo "built $rev"
done

# sign on the host, where gpg-agent holds the key
"$root/packaging/sign-changes.py" "$key" "$out"/*_source.changes

if [ "$upload" = "--upload" ]; then
    # dput checks the signatures before it sends anything
    keys=$(mktemp -d)
    gpg --export "${key%!}" > "$keys/key.gpg"
    docker run --rm -v "$out:/out" -v "$keys:/keys:ro" "$image" sh -c '
        apt-get update -qq >/dev/null
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq dput gnupg >/dev/null 2>&1
        gpg -q --import /keys/key.gpg 2>/dev/null
        cd /out && for c in *_source.changes; do dput '"$ppa"' "$c"; done'
    rm -r "$keys"
fi
