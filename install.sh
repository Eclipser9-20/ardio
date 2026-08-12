#!/bin/sh
# ardio installer
#
#   ./install.sh                    install to the default prefix
#   ./install.sh --prefix ~/.local  install somewhere else
#   ./install.sh --uninstall        remove an installed copy
#   ./install.sh --no-test          skip the test suite (not recommended)
#
# Default prefix is /usr/local when it is writable or sudo is available,
# otherwise ~/.local. Nothing is written outside the chosen prefix.

set -eu

PREFIX=""
RUN_TESTS=1
UNINSTALL=0
BUILD_DIR="build/install"

die() { printf 'error: %s\n' "$1" >&2; exit 1; }
info() { printf '==> %s\n' "$1"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a directory"
            PREFIX="$2"; shift 2 ;;
        --prefix=*)
            PREFIX="${1#--prefix=}"; shift ;;
        --uninstall)  UNINSTALL=1; shift ;;
        --no-test)    RUN_TESTS=0; shift ;;
        -h|--help)
            sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
done

# ---------------------------------------------------------------- prefix -----

# Returns 0 if we can write into a prefix, either directly or via sudo.
can_install_to() {
    dir="$1"
    # Walk up to the nearest existing ancestor -- that is what must be writable.
    while [ ! -d "$dir" ] && [ "$dir" != "/" ]; do
        dir=$(dirname "$dir")
    done
    [ -w "$dir" ]
}

SUDO=""
if [ -z "$PREFIX" ]; then
    if can_install_to /usr/local/bin; then
        PREFIX=/usr/local
    elif command -v sudo >/dev/null 2>&1; then
        PREFIX=/usr/local
        SUDO="sudo"
        info "/usr/local is not writable; will use sudo (or pass --prefix ~/.local)"
    else
        PREFIX="$HOME/.local"
        info "/usr/local is not writable and sudo is unavailable; using $PREFIX"
    fi
else
    case "$PREFIX" in "~"|"~/"*) PREFIX="$HOME${PREFIX#\~}" ;; esac
    can_install_to "$PREFIX" || SUDO="sudo"
fi

# ------------------------------------------------------------- uninstall -----

if [ "$UNINSTALL" -eq 1 ]; then
    removed=0
    for path in "$PREFIX/bin/ardio" "$PREFIX/lib/libardio.a" "$PREFIX/include/ardio"; do
        if [ -e "$path" ]; then
            $SUDO rm -rf "$path"
            printf '    removed %s\n' "$path"
            removed=1
        fi
    done
    [ "$removed" -eq 1 ] || info "nothing installed under $PREFIX"
    info "uninstalled"
    exit 0
fi

# ------------------------------------------------------------ toolchain ------

command -v cmake >/dev/null 2>&1 || die "cmake is required but was not found on PATH"

CMAKE_VERSION=$(cmake --version | head -1 | awk '{print $3}')
info "using cmake $CMAKE_VERSION"

case "$(uname -s)" in
    Darwin) info "platform: macOS (fully supported)" ;;
    Linux)  info "platform: Linux (serial layer not implemented yet; builds with stubs)" ;;
    *)      info "platform: $(uname -s) (serial layer not implemented yet; builds with stubs)" ;;
esac

# ---------------------------------------------------------------- build ------

cd "$(dirname "$0")"

info "configuring"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    >/dev/null || die "cmake configure failed"

info "building"
cmake --build "$BUILD_DIR" --parallel || die "build failed"

if [ "$RUN_TESTS" -eq 1 ]; then
    info "running tests"
    "$BUILD_DIR/ardio_tests" || die "tests failed -- refusing to install"
fi

info "installing to $PREFIX"
$SUDO cmake --install "$BUILD_DIR" >/dev/null || die "install failed"

# ----------------------------------------------------------------- done ------

printf '\n'
info "installed:"
printf '    %s\n' "$PREFIX/bin/ardio" "$PREFIX/lib/libardio.a" "$PREFIX/include/ardio/"

# Tell the user if the install location is not on their PATH.
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) printf '\nnote: %s is not on your PATH. Add it with:\n    export PATH="%s/bin:$PATH"\n' \
              "$PREFIX/bin" "$PREFIX" ;;
esac

printf '\nrun "ardio doctor" to check your setup.\n'
