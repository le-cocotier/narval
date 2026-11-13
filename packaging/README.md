# Release packages

The scripts that build what each `v*` tag's CI run attaches to the
[release](https://git.giorgio-nas.fr/arthur/narval/releases). Both
`.gitea/workflows/release-windows.yml` and `.github/workflows/release.yml`
call them, so the two releases hold the same files.

| Script                       | Builds                                                  |
|------------------------------|---------------------------------------------------------|
| `windows/build-zip.sh TAG`   | `narval-<tag>-windows-x64.zip` (cross-compiled)         |
| `source-tarball.sh VERSION`  | `narval-<version>.tar.gz`                               |
| `linux/build-packages.sh VERSION [OUTDIR] [amd64\|arm64]` | `narval_<version>_<arch>.deb` and `narval-<version>-1.<arch>.rpm` |

Each writes to `dist/` unless given another folder.

## How an installed narval finds its files

The `.deb` and `.rpm` (`meson install`) put `bin/narval` and
`share/narval/{config,packages,assets}`; the Windows zip has `config\`,
`packages\` and `assets\` next to `narval.exe`. narval looks for them from its
own path, through any symlink (`utilDataDir()` in `src/utils.c`;
`NARVAL_DATA_DIR` overrides it). On the first start it copies the default
`config/init.lisp`, a `personal-config` and the splash screen into `~/.narval`,
never overwriting anything. Bundled packages are loaded from the install, so
they upgrade with the editor; a package of the same name in
`~/.narval/packages` wins.

## Testing locally

```sh
packaging/linux/build-packages.sh 4.1.0-test /tmp/out   # this machine's architecture
sudo apt install /tmp/out/narval_4.1.0-test_amd64.deb
HOME=$(mktemp -d) narval --sync-packages                 # a first start: sets up ~/.narval
```
