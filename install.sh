#!/bin/bash
#
# Narval installer. Run with --help for usage.
#
# Everything runs from main(), called on the very last line: bash reads a
# script as it goes, so without that a `git pull` (--update) rewriting this
# file mid-run would have it carry on from a random point in the new one.

set -euo pipefail

# --- output helpers -----------------------------------------------------

if [ -t 1 ]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_BOLD=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_RESET=''
fi

info()  { echo "${C_BOLD}==>${C_RESET} $*"; }
ok()    { echo "${C_GREEN}[ok]${C_RESET} $*"; }
warn()  { echo "${C_YELLOW}[!] $*${C_RESET}"; }
error() { echo "${C_RED}[x] $*${C_RESET}" >&2; }
die()   { error "$@"; exit 1; }

# --- options --------------------------------------------------------------

ASSUME_YES=0
SKIP_PACKAGES=0
DEV_MODE=0
DEBUG_BUILD=0
UPDATE=0
UNINSTALL=0
PURGE=0
BINDIR="/usr/local/bin"
NARVAL_HOME="$HOME/.narval"
# The arguments to re-run with after --update, minus --update itself.
RERUN_ARGS=()

usage() {
    cat <<'EOF'
Narval installer.

Usage: ./install.sh [options]

  -y, --yes            Don't prompt for confirmation; assume "yes" (and
                        never overwrite an existing config).
      --bindir DIR      Where to install the narval binary (default:
                        /usr/local/bin). --prefix DIR is the old name for it.
      --skip-packages   Don't install the bundled packages (packages/ in
                        this checkout).
      --dev             Symlink the bundled packages to this checkout instead
                        of copying them, for working on them: edits there are
                        live in the editor and tracked by this repo's git.
      --update          `git pull` this checkout first, then install the
                        new version (with the other options given).
      --debug           Build an unoptimized debug build with APP_DEBUG logging
                        enabled, instead of the default optimized release
                        build. Logs (the editor's own trace calls, plus the
                        language server's own stderr chatter) go to
                        ~/.narval/debug.log, not the terminal — `tail -f`
                        it in another pane while you work.
      --uninstall       Remove the narval binary. ~/.narval (your config and
                        packages) is kept unless you also pass --purge.
      --purge           With --uninstall: delete ~/.narval as well.
  -h, --help            Show this help and exit.

Safe to re-run: an existing build/ is reconfigured rather than failing,
and your config is never touched without asking first. Nothing in
~/.narval changes until the build has succeeded. Bundled packages are
replaced to match this checkout; one with local edits (or an old git
clone of it) is moved to ~/.narval/packages.bak/ first, never deleted.
Packages that aren't bundled are never touched.

Your own changes belong in ~/.narval/packages/personal-config/init.lisp:
it's created from a template on the first install and never overwritten
afterwards (even with --yes or --skip-packages), so it survives updates.
EOF
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --update|--bindir|--bindir=*|--prefix|--prefix=*) ;;
            *) RERUN_ARGS+=("$1") ;;
        esac
        case "$1" in
            -y|--yes) ASSUME_YES=1 ;;
            --bindir|--prefix) BINDIR="${2:?$1 needs a directory}"; shift ;;
            --bindir=*|--prefix=*) BINDIR="${1#*=}" ;;
            --skip-packages) SKIP_PACKAGES=1 ;;
            --dev) DEV_MODE=1 ;;
            --update) UPDATE=1 ;;
            --debug) DEBUG_BUILD=1 ;;
            --uninstall) UNINSTALL=1 ;;
            --purge) PURGE=1 ;;
            -h|--help) usage; exit 0 ;;
            *) die "Unknown option: $1 (see --help)" ;;
        esac
        shift
    done
    if [ "$UPDATE" -eq 1 ] && [ "$UNINSTALL" -eq 1 ]; then
        die "--update and --uninstall don't go together"
    fi
    if [ "$PURGE" -eq 1 ] && [ "$UNINSTALL" -eq 0 ]; then
        die "--purge only makes sense with --uninstall"
    fi
    # A relative --bindir would otherwise be resolved against the checkout
    # (we cd there), not where you ran the script from.
    case "$BINDIR" in
        /*) ;;
        *) BINDIR="$PWD/$BINDIR" ;;
    esac
    RERUN_ARGS+=(--bindir "$BINDIR")
}

confirm() {
    # confirm "question" — returns success (0) for yes.
    [ "$ASSUME_YES" -eq 1 ] && return 0
    local reply
    read -r -p "$1 (y/N) " reply || return 1
    [[ "$reply" =~ ^[Yy]$ ]]
}

as_root() {
    # as_root <command...> — runs it with sudo unless we're root already.
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo &>/dev/null; then
        sudo "$@"
    else
        die "Need root for: $* — and sudo isn't installed. Re-run as root, or pick a --bindir you can write to."
    fi
}

# --- dependencies ----------------------------------------------------------

detect_pkg_manager() {
    # On macOS that's Homebrew; elsewhere the system's own manager comes
    # first, even when Homebrew (Linuxbrew) is installed too.
    if [ "$(uname -s)" = "Darwin" ]; then
        if command -v brew &>/dev/null; then echo "brew"; else echo "unknown"; fi
        return
    fi
    if command -v apt-get &>/dev/null; then echo "apt"
    elif command -v dnf &>/dev/null; then echo "dnf"
    elif command -v pacman &>/dev/null; then echo "pacman"
    elif command -v zypper &>/dev/null; then echo "zypper"
    elif command -v apk &>/dev/null; then echo "apk"
    elif command -v brew &>/dev/null; then echo "brew"
    else echo "unknown"
    fi
}

package_for() {
    # package_for <pkg manager> <dependency> — the package providing it there
    case "$2" in
        ninja)
            case "$1" in
                apt|dnf) echo "ninja-build" ;;
                apk)     echo "samurai" ;;
                *)       echo "ninja" ;;
            esac ;;
        cc)
            case "$1" in
                apt)    echo "build-essential" ;;
                pacman) echo "base-devel" ;;
                apk)    echo "build-base" ;;
                *)      echo "gcc" ;;
            esac ;;
        *) echo "$2" ;;
    esac
}

install_cmd_for() {
    # install_cmd_for <pkg manager> <package>
    case "$1" in
        brew)   echo "brew install $2" ;;
        apt)    echo "apt-get update && apt-get install -y $2" ;;
        dnf)    echo "dnf install -y $2" ;;
        pacman) echo "pacman -S --needed --noconfirm $2" ;;
        zypper) echo "zypper --non-interactive install $2" ;;
        apk)    echo "apk add $2" ;;
    esac
}

have_dep() {
    # have_dep <dependency>, where cc means "any C compiler"
    if [ "$1" = "cc" ]; then
        command -v cc &>/dev/null || command -v gcc &>/dev/null || command -v clang &>/dev/null
    else
        command -v "$1" &>/dev/null
    fi
}

check_dep() {
    local dep="$1"
    have_dep "$dep" && return 0

    if [ "$dep" = "cc" ]; then
        error "No C compiler found (cc/gcc/clang)"
        if [ "$(uname -s)" = "Darwin" ]; then
            die "Install Apple's command line tools with \`xcode-select --install\`, then re-run this script."
        fi
    else
        error "Missing dependency: $dep"
    fi

    local mgr pkg cmd shown
    mgr="$(detect_pkg_manager)"
    if [ "$mgr" = "unknown" ]; then
        die "Please install '$dep' and re-run this script."
    fi
    pkg="$(package_for "$mgr" "$dep")"
    cmd="$(install_cmd_for "$mgr" "$pkg")"
    # Homebrew refuses to run as root; every other manager needs it.
    if [ "$mgr" = "brew" ]; then shown="$cmd"; else shown="sudo sh -c '$cmd'"; fi
    if confirm "Run \`$shown\` now to install it?"; then
        if [ "$mgr" = "brew" ]; then sh -c "$cmd"; else as_root sh -c "$cmd"; fi
        have_dep "$dep" || die "'$dep' still not found after installing — install it manually and re-run."
    else
        die "Install it yourself, e.g.: $shown"
    fi
}

check_deps() {
    info "Checking dependencies ..."
    check_dep meson
    check_dep ninja
    check_dep git
    check_dep cc
    ok "All dependencies present"
}

# --- update ----------------------------------------------------------------

update_and_rerun() {
    [ -d .git ] || die "--update needs a git clone of narval, and $SCRIPT_DIR isn't one."
    info "Updating $SCRIPT_DIR ..."
    local before
    before="$(git rev-parse HEAD)"
    git pull --ff-only || die "git pull failed (local changes or a diverged branch?) — sort it out and re-run."
    if [ "$(git rev-parse HEAD)" = "$before" ]; then
        ok "Already up to date"
    else
        git --no-pager log --oneline "$before..HEAD"
    fi
    # Re-run the (possibly new) installer, so a changed install.sh is the
    # one doing the installing.
    exec bash "$SCRIPT_DIR/install.sh" ${RERUN_ARGS[@]+"${RERUN_ARGS[@]}"}
}

# --- uninstall -------------------------------------------------------------

uninstall() {
    local bin="$BINDIR/narval"
    if [ ! -e "$bin" ]; then
        # Not where --bindir says: the one on PATH, then.
        bin="$(command -v narval 2>/dev/null || true)"
    fi

    echo "${C_BOLD}--- Narval uninstaller ---${C_RESET}"
    if [ -n "$bin" ] && [ -e "$bin" ]; then
        if confirm "Remove $bin?"; then
            if [ -w "$(dirname "$bin")" ]; then rm -f "$bin"; else as_root rm -f "$bin"; fi
            ok "Removed $bin"
        fi
    else
        warn "No narval binary found in $BINDIR or on your PATH"
    fi

    if [ ! -d "$NARVAL_HOME" ]; then
        return 0
    elif [ "$PURGE" -eq 1 ]; then
        if confirm "Delete $NARVAL_HOME (your config, personal-config and every package) for good?"; then
            rm -rf "$NARVAL_HOME"
            ok "Deleted $NARVAL_HOME"
        fi
    else
        echo "Kept $NARVAL_HOME (your config and packages); --uninstall --purge deletes it too."
    fi
}

# --- build ------------------------------------------------------------------

build() {
    local meson_args
    if [ "$DEBUG_BUILD" -eq 1 ]; then
        info "Building (debug, APP_DEBUG logging enabled) ..."
        meson_args=(-Dbuildtype=debug -Dc_args=-DAPP_DEBUG)
    else
        info "Building (release) ..."
        # -Dc_args= explicitly: `meson setup --reconfigure` keeps every option
        # it isn't given, so after one --debug install the -DAPP_DEBUG from it
        # silently stayed in every "release" build that followed.
        meson_args=(-Dbuildtype=release -Dc_args=)
    fi

    # --reconfigure only on a directory meson has actually set up: on an
    # empty or half-created build/ it fails.
    if [ -d build/meson-private ]; then
        meson setup --reconfigure build "${meson_args[@]}" || die "meson setup failed"
    else
        meson setup build "${meson_args[@]}" || die "meson setup failed"
    fi
    meson compile -C build || die "Build failed — nothing was installed, ~/.narval is untouched."
}

# --- migrating from beluga ----------------------------------------------------

# The editor was called beluga: carry ~/.beluga over (config, packages,
# projects) unless ~/.narval already exists. narval itself does the same
# on its first start; doing it here keeps install_config from setting up a
# fresh ~/.narval beside the old one.
migrate_home() {
    local legacy="$HOME/.beluga"
    if [ ! -e "$NARVAL_HOME" ] && [ -d "$legacy" ]; then
        mv "$legacy" "$NARVAL_HOME" || die "Couldn't move $legacy to $NARVAL_HOME"
        ok "Moved $legacy to $NARVAL_HOME (narval was called beluga)"
    fi
    rm -f "$NARVAL_HOME/assets/beluga.txt"
}

# Removes the beluga binary an earlier install put in BINDIR: narval
# replaces it.
remove_old_binary() {
    local old="$BINDIR/beluga"
    [ -e "$old" ] || [ -L "$old" ] || return 0
    if [ -w "$BINDIR" ]; then
        rm -f "$old"
    else
        as_root rm -f "$old"
    fi
    ok "Removed the old $old (beluga is now narval)"
}

# --- config files ----------------------------------------------------------

install_config() {
    local config="$NARVAL_HOME/config/init.lisp"

    info "Setting up $NARVAL_HOME ..."
    mkdir -p "$NARVAL_HOME/assets" "$NARVAL_HOME/packages" "$NARVAL_HOME/config"
    cp -f ./assets/narval.txt "$NARVAL_HOME/assets/"

    if [ -f "$config" ]; then
        if cmp -s ./config/init.lisp "$config"; then
            echo "  config already matches the default"
        elif [ "$ASSUME_YES" -eq 1 ]; then
            cp -f ./config/init.lisp "$config.new"
            warn "Kept your existing config; new default written to $config.new"
        elif confirm "Replace your existing config (old one saved as init.lisp.bak)?"; then
            mv -f "$config" "$config.bak"
            cp -f ./config/init.lisp "$config"
            ok "Config replaced (backup at $config.bak)"
        else
            cp -f ./config/init.lisp "$config.new"
            ok "Kept your config; new default written to $config.new"
        fi
    else
        cp -f ./config/init.lisp "$config"
        ok "Config installed at $config"
    fi

    # personal-config: the one package that's yours rather than a git clone —
    # created once from the template, then never overwritten (not even with
    # --yes or on re-install), so edits there survive every update. Independent
    # of --skip-packages for the same reason.
    PERSONAL_CONFIG="$NARVAL_HOME/packages/personal-config/init.lisp"
    if [ -f "$PERSONAL_CONFIG" ]; then
        echo "  personal-config already exists, leaving it alone"
    else
        mkdir -p "$(dirname "$PERSONAL_CONFIG")"
        cp ./config/personal-config.lisp "$PERSONAL_CONFIG"
        ok "Personal config created at $PERSONAL_CONFIG"
    fi
    # A config from before personal-config existed never loads it, so edits
    # there would silently do nothing.
    if ! grep -q '^[[:space:]]*(add-package[[:space:]]*"personal-config")' "$config"; then
        warn "$config doesn't load personal-config — add (add-package \"personal-config\") after the theme's add-package line"
    fi
    # use-package comes from the package manager: a config from before it
    # existed doesn't load it, and every use-package line would fail.
    if ! grep -q '^[[:space:]]*(add-package[[:space:]]*"package")' "$config"; then
        warn "$config doesn't load the package manager — add (add-package \"package\") before the other add-package lines (see config/init.lisp)"
    fi
}

# --- packages ----------------------------------------------------------

# Bundled packages ship in this checkout's packages/ directory, versioned
# with the editor itself: they call builtins that only exist from a given
# version on (dired's process-run, outline's editor-recenter), so they're
# installed from the same checkout as the binary instead of being cloned
# from repos of their own that could drift out of step with it. A normal
# install copies them; --dev symlinks each one to the checkout instead, so
# editing a package there is live in the editor and shows up in this
# repo's git status right next to the C changes.
#
# ~/.narval/packages/<name> for a bundled package is narval's to replace on
# every install — that's what keeps it in step with the binary. Anything
# that would be lost doing so (a copy with local edits, or an old git
# clone from before packages were bundled) is moved to
# ~/.narval/packages.bak/<name>-<timestamp> first, never deleted. Packages
# that aren't bundled — personal-config, your own, third-party ones — are
# never touched.

backup_package() {
    # backup_package <installed dir> <name> <why>
    mkdir -p "$PACKAGES_BAK"
    mv "$1" "$PACKAGES_BAK/$2-$STAMP"
    warn "'$2' $3 — moved to $PACKAGES_BAK/$2-$STAMP"
}

install_package() {
    # install_package <name> — prints nothing when already up to date
    local name="$1"
    local src="$PACKAGES_SRC/$1"
    local dest="$NARVAL_HOME/packages/$1"

    if [ -L "$dest" ]; then
        if [ "$DEV_MODE" -eq 1 ] && [ "$(readlink "$dest")" = "$src" ]; then
            return 0
        fi
        rm "$dest"   # a symlink holds nothing of its own to lose
    elif [ -d "$dest/.git" ]; then
        backup_package "$dest" "$name" "was a git clone (now bundled)"
    elif [ -d "$dest" ]; then
        if diff -rq -x lisp_log.err -x .DS_Store -x .bundled "$src" "$dest" >/dev/null 2>&1; then
            if [ "$DEV_MODE" -eq 0 ]; then
                : > "$dest/.bundled"   # an install from before the marker
                return 0
            fi
            rm -rf "$dest"   # identical to the bundled copy: nothing to keep
        else
            backup_package "$dest" "$name" "had local changes"
        fi
    elif [ -e "$dest" ]; then
        backup_package "$dest" "$name" "wasn't a directory"
    fi

    if [ "$DEV_MODE" -eq 1 ]; then
        ln -s "$src" "$dest"
        echo "  $name -> $src"
    else
        cp -R "$src" "$dest"
        # Marks it as narval's own (the *packages* list shows it as
        # "bundled"); a --dev link is recognised as a link instead.
        : > "$dest/.bundled"
        echo "  $name"
    fi
}

install_packages() {
    PACKAGES_SRC="$SCRIPT_DIR/packages"
    PACKAGES_BAK="$NARVAL_HOME/packages.bak"
    STAMP="$(date +%Y%m%d-%H%M%S)"

    if [ "$SKIP_PACKAGES" -eq 1 ]; then
        info "Skipping bundled packages (--skip-packages)"
        return 0
    elif [ ! -d "$PACKAGES_SRC" ]; then
        warn "No packages/ directory in this checkout — skipping bundled packages"
        return 0
    fi

    if [ "$DEV_MODE" -eq 1 ]; then
        info "Linking bundled packages to $PACKAGES_SRC (--dev) ..."
    else
        info "Installing bundled packages ..."
    fi
    local pkg_dir
    for pkg_dir in "$PACKAGES_SRC"/*/; do
        install_package "$(basename "$pkg_dir")"
    done
    ok "Bundled packages up to date"
}

# --- install the binary ----------------------------------------------------------

install_binary() {
    info "Installing to $BINDIR ..."
    if [ ! -d "$BINDIR" ]; then
        mkdir -p "$BINDIR" 2>/dev/null || as_root mkdir -p "$BINDIR"
    fi
    # Copy next to it, then rename over it: writing into the file in place
    # breaks a narval that's running from it (macOS kills it outright).
    if [ -w "$BINDIR" ]; then
        cp -f ./build/narval "$BINDIR/.narval.new"
        mv -f "$BINDIR/.narval.new" "$BINDIR/narval"
    else
        warn "$BINDIR isn't writable by you — using sudo"
        as_root cp -f ./build/narval "$BINDIR/.narval.new"
        as_root mv -f "$BINDIR/.narval.new" "$BINDIR/narval"
    fi

    echo
    ok "Installed $("$BINDIR/narval" --version 2>/dev/null || echo narval) to $BINDIR/narval"
    local found
    found="$(command -v narval 2>/dev/null || true)"
    if [ -z "$found" ]; then
        warn "$BINDIR isn't on your PATH yet — add it, e.g.: export PATH=\"$BINDIR:\$PATH\""
    elif [ "$found" != "$BINDIR/narval" ]; then
        warn "\`narval\` on your PATH is $found, not the one just installed — remove it or put $BINDIR first in PATH"
    fi
    echo "Put your customizations in $PERSONAL_CONFIG — unlike"
    echo "$NARVAL_HOME/config/init.lisp, it's never replaced by an update."
}

# --- main ----------------------------------------------------------------

main() {
    parse_args "$@"

    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$SCRIPT_DIR"

    if [ ! -f "./meson.build" ] || [ ! -f "./assets/narval.txt" ]; then
        die "This doesn't look like a narval checkout ($SCRIPT_DIR). Run this script from inside the cloned repo."
    fi

    if [ "$UNINSTALL" -eq 1 ]; then
        uninstall
        exit 0
    fi
    if [ "$UPDATE" -eq 1 ]; then
        update_and_rerun   # doesn't return
    fi

    echo "${C_BOLD}--- Narval installer ---${C_RESET}"
    confirm "Install narval from $SCRIPT_DIR to $BINDIR?" || { echo "Aborted."; exit 1; }

    check_deps
    # Build first: if it fails, ~/.narval stays as it was, rather than
    # holding new packages the old binary can't run.
    build
    migrate_home
    install_config
    install_packages
    install_binary
    remove_old_binary
}

main "$@"
