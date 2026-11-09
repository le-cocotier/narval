# Packaging narval

Recipes for the package managers narval is published to. Everything here
uses the files a `v*` tag's CI run attaches to the
[release](https://git.giorgio-nas.fr/arthur/narval/releases):

| File                                  | Made by            | Used by                      |
|---------------------------------------|--------------------|------------------------------|
| `narval-<version>.tar.gz` (source)    | `linux-packages`   | Homebrew, the AUR            |
| `narval-v<version>-windows-x64.zip`   | `windows-package`  | winget                       |
| `narval_<version>_amd64.deb`          | `linux-packages`   | Gitea's Debian registry      |
| `narval-<version>-1.x86_64.rpm`       | `linux-packages`   | Gitea's RPM registry         |

The jobs are in `.gitea/workflows/release-windows.yml`. The `.deb` and `.rpm`
go to the Gitea registries on their own; the other three need the steps
below once per release.

## How an installed narval finds its files

A package manager installs `bin/narval` and `share/narval/{config,packages,assets}`
(`meson install`); the Windows zip has `config\`, `packages\` and `assets\`
next to `narval.exe`. narval looks for them from its own path, through any
symlink (`utilDataDir()` in `src/utils.c`; `NARVAL_DATA_DIR` overrides it).
On the first start it copies the default `config/init.lisp`, a
`personal-config` and the splash screen into `~/.narval`, never overwriting
anything. Bundled packages are loaded from the install, so they upgrade
with the editor; a package of the same name in `~/.narval/packages` wins.

A narval set up by `install.sh` / `install.ps1`, or run from a build
directory, finds no data directory and behaves exactly as before.

## Each release

1. Tag and push: `git tag v3.5 && git push origin v3.5`. Wait for the
   *Release packages* run to finish.
2. `packaging/render.sh 3.5`. It downloads the tarball and the zip, hashes
   them, and writes filled-in recipes to `dist/packaging/3.5/`.
3. Publish them:

### Homebrew (tap)

Once: create an empty public repository `arthur/homebrew-narval` on
git.giorgio-nas.fr. Each release:

```sh
git clone https://git.giorgio-nas.fr/arthur/homebrew-narval.git
cp -r dist/packaging/3.5/homebrew-narval/Formula homebrew-narval/
cd homebrew-narval
brew install --build-from-source ./Formula/narval.rb && brew test narval && brew audit --strict --formula ./Formula/narval.rb
git add Formula && git commit -m "narval 3.5" && git push
```

Users: `brew tap arthur/narval https://git.giorgio-nas.fr/arthur/homebrew-narval.git && brew install narval`.
A tap that isn't on GitHub needs the URL in `brew tap`; `brew install arthur/narval/narval`
alone only works for GitHub taps.

homebrew-core (plain `brew install narval`, no tap) is possible later: it
wants a stable, notable project (it judges stars, forks and users), an
OSI license and a formula that passes `brew audit --new --strict`. The
formula here is written to that standard.

### winget

The manifest installs the release zip as a portable app (`narval` on
PATH). Needs a GitHub account. From Windows:

```powershell
winget validate --manifest dist\packaging\3.5\winget-pkgs\manifests\a\ArthurBarraux\Narval\3.5
winget install --manifest dist\packaging\3.5\winget-pkgs\manifests\a\ArthurBarraux\Narval\3.5
narval --version
```

Then fork https://github.com/microsoft/winget-pkgs, copy
`manifests/a/ArthurBarraux/Narval/3.5/` into it and open a pull request
(or `wingetcreate submit <that folder>`). Microsoft's bots download the zip
and scan it, so git.giorgio-nas.fr must be reachable from the internet and
the file must never change after the PR. For later versions,
`wingetcreate update ArthurBarraux.Narval --version 3.6 --urls <zip url> --submit`
does it in one step.

### AUR

Needs an AUR account with your SSH key. The package is `narval-editor`;
check on https://aur.archlinux.org that the name is free first. From Arch
(or an Arch container):

```sh
git clone ssh://aur@aur.archlinux.org/narval-editor.git
cp dist/packaging/3.5/aur/narval-editor/PKGBUILD narval-editor/
cd narval-editor
makepkg -si                          # builds, runs the tests, installs
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO && git commit -m "narval 3.5" && git push
```

### Debian / RPM registries

Nothing to do: CI uploads them. Users add the repository once (README.md,
*Install*). Gitea signs the repository metadata with its own key; the
`repository.key` URL serves it.

## Testing locally

```sh
packaging/linux/build-packages.sh 3.5-test /tmp/out   # the .deb and .rpm, from this checkout
sudo apt install /tmp/out/narval_3.5-test_amd64.deb
HOME=$(mktemp -d) narval --sync-packages               # a first start: sets up ~/.narval
```
