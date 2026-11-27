#!/usr/bin/env bash
# Builds the Arch package (packaging/arch/PKGBUILD) into a pacman
# repository: OUTDIR gets the package and the database, puddlejumper.db
# (real files, not symlinks: GitHub release assets can't be links).
#
#   packaging/arch/build-repo.sh OUTDIR
#
# With GPG_KEY set (a key id in gpg's keyring), the package and the
# database are signed with it. makepkg won't run as root, so as root (in CI)
# it builds as the user BUILD_USER (default: builder, made if missing).
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=$(mkdir -p "${1:?usage: build-repo.sh OUTDIR}" && cd -- "$1" && pwd)
repo=${REPO_NAME:-puddlejumper}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$here/PKGBUILD" "$work/"
# The PKGBUILD finds the checkout from where it is: tell it.
sed -i "s|^_root=.*|_root=$(cd "$here/../.." && pwd)|" "$work/PKGBUILD"

if [[ $(id -u) == 0 ]]; then
  user=${BUILD_USER:-builder}
  id "$user" >/dev/null 2>&1 || useradd -m "$user"
  # The checkout too: git (pkgver, Go's VCS stamp) only trusts a repository
  # its user owns, and a CI checkout belongs to someone else.
  chown -R "$user" "$work" "$out" "$(cd "$here/../.." && pwd)"
  (cd "$work" && runuser -u "$user" -- env PKGDEST="$out" makepkg --noconfirm --nodeps)
else
  (cd "$work" && PKGDEST="$out" makepkg --noconfirm --syncdeps)
fi

cd "$out"
shopt -s nullglob
pkgs=(*.pkg.tar.zst)
[[ ${#pkgs[@]} -gt 0 ]] || { echo "no package was built" >&2; exit 1; }
sign=()
if [[ -n ${GPG_KEY:-} ]]; then
  for p in "${pkgs[@]}"; do
    gpg --batch --yes --local-user "$GPG_KEY" --detach-sign --no-armor "$p"
  done
  sign=(--sign --key "$GPG_KEY")
fi
rm -f "$repo".db* "$repo".files*
repo-add "${sign[@]}" "$repo.db.tar.zst" "${pkgs[@]}"
# The names pacman asks for, as files.
for kind in db files; do
  rm -f "$repo.$kind" "$repo.$kind.sig"
  cp "$repo.$kind.tar.zst" "$repo.$kind"
  [[ -f "$repo.$kind.tar.zst.sig" ]] && cp "$repo.$kind.tar.zst.sig" "$repo.$kind.sig"
done
ls -l "$out"
