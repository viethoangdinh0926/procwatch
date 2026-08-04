#!/bin/bash
# Builds the injector for musl (Alpine) into build/agent/<arch>-musl/lib/.
#
# Alpine images need this artifact: the glibc build is not a usable fallback.
# Preloaded into a musl process it cannot load at all, because its DT_NEEDED
# names libc.so.6, which does not exist there. Both loaders ignore an object
# they cannot open, so the container keeps working, but the constructor never
# runs and the workload is silently left uninstrumented, which is a tedious
# thing to notice and diagnose.
#
# The genuinely dangerous case is different and applies to both libcs equally:
# an object that loads but cannot resolve a symbol aborts the process with a
# relocation error. --no-undefined below is what prevents that.
#
# Runs inside an Alpine container rather than dockcross because we need musl
# headers and a musl-targeting compiler.
#
# Usage: build_inject_musl.sh [arch]
set -e

ARCH="${1:-x86_64}"
DOCKER="${DOCKER:-docker}"
OUTDIR="build/agent/${ARCH}-musl"
HOST_UID="${HOST_UID:-$(id -u)}"
HOST_GID="${HOST_GID:-$(id -g)}"

case "$ARCH" in
    x86_64)  PLATFORM="linux/amd64" ;;
    aarch64) PLATFORM="linux/arm64" ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

# All filesystem work happens inside the container: the dockcross builds run
# as root, so build/agent/ is root-owned and a host-side mkdir would fail.
$DOCKER run --rm --platform "$PLATFORM" -v "$(pwd):/work" -w /work alpine:3.20 /bin/sh -c '
set -e
apk add --no-cache build-base binutils >/dev/null

OUTDIR="'"$OUTDIR"'"
mkdir -p "$OUTDIR/lib"

gcc -O2 -Wall -Wextra -Iinclude -fPIC -shared -fvisibility=hidden \
    src/inject/inject.c src/inject/detect.c src/inject/envmod.c \
    src/inject/metrics_thread.c src/inject/exec_wrap.c src/common/proc_push.c \
    -Wl,--version-script=src/inject/inject.map \
    -Wl,--no-undefined -Wl,-z,now -Wl,-z,relro \
    -o "$OUTDIR/lib/libprocwatch_inject.so"

# musl exposes getauxval, dlsym, and pthread from libc proper, so libc should
# still be the only dependency.
needed=$(readelf -d "$OUTDIR/lib/libprocwatch_inject.so" |
         sed -n "s/.*Shared library: \[\(.*\)\]/\1/p" |
         grep -v "^libc\.musl" | grep -v "^libc\.so" || true)
if [ -n "$needed" ]; then
    echo "error: musl injector depends on more than libc: $needed" >&2
    exit 1
fi
bad_exports=$(nm -D --defined-only "$OUTDIR/lib/libprocwatch_inject.so" 2>/dev/null |
              awk "{print \$3}" |
              grep -vE "^(__libc_start_main|execve|execvpe|execveat)$" || true)
if [ -n "$bad_exports" ]; then
    echo "error: musl injector exports unexpected symbols:" >&2
    echo "$bad_exports" >&2
    exit 1
fi

mkdir -p "$OUTDIR/bin"
# Fully static so the same binary works on Alpine (Go samples) and glibc hosts.
gcc -O2 -Wall -Wextra -Iinclude -static \
    src/wrap/wrap.c src/common/proc_push.c src/proc_scan.c src/util.c \
    -o "$OUTDIR/bin/procwatch-wrap"

# Prefer this portable wrap in the primary agent tree as well.
PRIMARY="build/agent/'"$ARCH"'"
mkdir -p "$PRIMARY/bin"
cp -f "$OUTDIR/bin/procwatch-wrap" "$PRIMARY/bin/procwatch-wrap"

# Hand the tree back to the invoking user; the container runs as root.
if [ -n "'"${HOST_UID:-}"'" ]; then
    chown -R "'"${HOST_UID:-0}"':'"${HOST_GID:-0}"'" build 2>/dev/null || true
fi
'

echo "Built $OUTDIR/lib/libprocwatch_inject.so and $OUTDIR/bin/procwatch-wrap (musl static)"
