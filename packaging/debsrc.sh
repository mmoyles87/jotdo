#!/bin/sh
# Lay out a Debian source tree for a release: the orig tarball from git, one orig-<component>
# tarball per entry in packaging/sources (checked against its sha256), each unpacked into
# <component>/, and debian/ copied in. Used by ppa.sh and the release workflow.
#
#   packaging/debsrc.sh VERSION OUT [REF]     REF defaults to vVERSION
#
# Leaves OUT/jotdo_VERSION.orig*.tar.gz and OUT/jotdo-VERSION/.
set -eu

version=${1:?usage: debsrc.sh VERSION OUT [REF]}
out=${2:?usage: debsrc.sh VERSION OUT [REF]}
ref=${3:-v$version}
name=jotdo
root=$(git rev-parse --show-toplevel)
tree="$out/$name-$version"

mkdir -p "$out"
rm -rf "$tree"
git -C "$root" archive --prefix="$name-$version/" -o "$out/${name}_$version.orig.tar.gz" "$ref"
tar xzf "$out/${name}_$version.orig.tar.gz" -C "$out"

grep -v '^#' "$root/packaging/sources" | while read -r component url sha; do
    [ -n "$component" ] || continue
    tarball="$out/${name}_$version.orig-$component.tar.gz"
    [ -f "$tarball" ] || curl -sSfL -o "$tarball" "$url"
    echo "$sha  $tarball" | sha256sum -c --quiet
    mkdir -p "$tree/$component"
    tar xzf "$tarball" -C "$tree/$component" --strip-components=1
done

cp -r "$root/packaging/debian" "$tree/debian"
