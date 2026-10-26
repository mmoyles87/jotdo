# Releasing

1. Set the new version in `CMakeLists.txt` (`project(... VERSION ...)`), `doc/jotdo.1` (the
   `.TH` line), `packaging/rpm/jotdo.spec` (`Version`, and a `%changelog` entry),
   `packaging/debian/changelog` (a new entry on top) and `packaging/aur/jotdo/PKGBUILD`
   (`pkgver`, `pkgrel=1`). Update the version in the README's install links.
   A dependency bump touches `CMakeLists.txt`, `packaging/sources`, the spec, the PKGBUILD
   and the Homebrew formula.
2. Build and test, commit, then tag and push:
   `git tag -s vX.Y.Z -m vX.Y.Z && git push origin main vX.Y.Z`.
   The release workflow builds `.deb`s (on Ubuntu 24.04) and `.rpm`s (on Fedora 43) for
   x86_64 and aarch64 from the same packaging the PPA and COPR use, and publishes them with
   `SHA256SUMS`.
3. AUR, once the release is up: in `packaging/aur/jotdo`, run
   `updpkgsums && makepkg --printsrcinfo > .SRCINFO`, build it with `makepkg -fC`, then copy
   `PKGBUILD` and `.SRCINFO` into the AUR checkout
   (`git clone ssh://aur@aur.archlinux.org/jotdo.git`), commit and push.
4. PPA: `packaging/ppa.sh X.Y.Z --upload` builds, signs and uploads a source package for
   each supported Ubuntu series, with the bundled dependencies as extra orig tarballs
   (`packaging/sources`). Launchpad builds them in an hour or so.
5. Homebrew: in `mmoyles87/homebrew-tap`, set `url` and `sha256` in `Formula/jotdo.rb` to the
   new tag's tarball
   (`curl -sL https://github.com/mmoyles87/jotdo/archive/refs/tags/vX.Y.Z.tar.gz | sha256sum`),
   then `brew install --build-from-source mmoyles87/tap/jotdo && brew test jotdo && brew audit --strict --online mmoyles87/tap/jotdo`
   on a Mac, commit and push.
6. COPR: build the source RPM from `packaging/rpm/jotdo.spec` (`spectool -g` then
   `rpmbuild -bs`) and submit it with `copr-cli build mmoyles87/jotdo jotdo-X.Y.Z-1.*.src.rpm`.
   `copr-cli` reads its token from `~/.config/copr`.
