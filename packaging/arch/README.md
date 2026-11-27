# Arch Linux

`PKGBUILD` builds PuddleJumper from this checkout. CI (`.github/workflows/arch.yml`)
builds it on every push to `main` into a pacman repository and publishes it
as the assets of the GitHub release `arch-x86_64`. pacman can read a release
like any other repository.

## Installing from the repository

Add the repository to `/etc/pacman.conf`:

```ini
[puddlejumper]
Server = https://github.com/undefinedopcode/puddlejumper/releases/download/arch-$arch
```

The repository is signed, so pacman has to trust its key. Do this once:

```sh
curl -fsSL https://github.com/undefinedopcode/puddlejumper/releases/download/arch-x86_64/puddlejumper.asc | sudo pacman-key --add -
sudo pacman-key --lsign-key 5C92C7FD64DAB8EFD20667A0F456F750ECB36FB4
sudo pacman -Sy puddlejumper
```

The key is `undefinedopcode packages`, fingerprint
`5C92C7FD64DAB8EFD20667A0F456F750ECB36FB4` (ed25519, expires 2029-11-26).
Check that `pacman-key --add` printed this fingerprint before you sign it.

atlantis has a repository of its own, signed with the same key and set up
the same way:

```ini
[atlantis]
Server = https://github.com/undefinedopcode/atlantis/releases/download/arch-$arch
```

## Building it yourself

```sh
cd packaging/arch && makepkg -si               # build and install
packaging/arch/build-repo.sh /tmp/repo          # or build a repository
```

The version is the one in `CMakeLists.txt`, plus the commit count and hash
(`0.2.0.r812.gabcdef0`). Every push therefore produces a newer package, and
`pacman -Syu` picks it up.

## Signing (maintainers)

The CI signs packages with the key in the secret `ARCH_SIGNING_KEY`. Without
that secret it publishes them unsigned. To set the key up:

```sh
export GNUPGHOME=$(mktemp -d)
gpg --batch --passphrase '' --quick-gen-key 'PuddleJumper packages <...>' ed25519 sign 3y
gpg --armor --export-secret-keys | gh secret set ARCH_SIGNING_KEY -R undefinedopcode/puddlejumper
gpg --armor --export > puddlejumper.asc        # publish this, with its fingerprint
```

Use a key made only for this, with no passphrase (CI can't type one). Keep
a backup of it offline. When it expires or leaks, make a new one and publish
the new public key.
