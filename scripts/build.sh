#!/bin/bash
# Cross-compiles procwatch inside a dockcross container and packages it as a
# self-contained bundle: build/<arch>/procwatch + build/<arch>/lib/*.so
#
# Usage: build.sh <arch: x86_64|aarch64> <cc> [cflags...]
set -e

ARCH="$1"
CC="$2"
shift 2
CFLAGS=("$@")

SRCS=(src/main.c src/util.c src/proc_scan.c src/db.c)
OUTDIR="build/$ARCH"

apt-get update
if [ "$ARCH" = "aarch64" ]; then
    dpkg --add-architecture arm64
    apt-get update
    apt-get install -y gcc-aarch64-linux-gnu libpq-dev:arm64 libssl-dev:arm64 zlib1g-dev:arm64
    LIBDIRS=(/usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu)
else
    apt-get install -y libpq-dev libssl-dev zlib1g-dev
    LIBDIRS=(/usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu)
fi

mkdir -p "$OUTDIR/lib"

# -rpath '$ORIGIN/lib' (with --disable-new-dtags, i.e. classic DT_RPATH rather
# than DT_RUNPATH) lets the binary find its bundled libs at runtime, and-
# crucially-propagates to libraries loaded transitively (e.g. libpq's own
# dependencies on libssl/libcrypto/libgssapi_krb5/libldap/...), which
# DT_RUNPATH would not do.
"$CC" "${CFLAGS[@]}" -I/usr/include/postgresql "${SRCS[@]}" \
    -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/lib' \
    -lpq -lssl -lcrypto -lz -pthread \
    -o "$OUTDIR/procwatch"

# Bundle the full transitive closure of shared libs (excluding libc/ld-linux,
# which are assumed to always be present as part of the base OS/glibc).
#
# We resolve NEEDED entries with `readelf` (pure ELF header parsing) instead
# of `ldd` (which loads/executes the binary via the dynamic linker) because
# for aarch64 this script cross-compiles on an x86_64 host: `ldd` can't run a
# foreign-architecture binary ("not a dynamic executable"), but `readelf`
# works fine on any ELF regardless of host/target architecture.
declare -A seen=()
queue=("$OUTDIR/procwatch")
while [ "${#queue[@]}" -gt 0 ]; do
    cur="${queue[0]}"
    queue=("${queue[@]:1}")
    needed=$(readelf -d "$cur" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
    for name in $needed; do
        case "$name" in
            libc.so*|ld-linux*) continue ;;
        esac
        [ -n "${seen[$name]:-}" ] && continue
        seen[$name]=1
        found=""
        for d in "${LIBDIRS[@]}"; do
            if [ -e "$d/$name" ]; then found="$d/$name"; break; fi
        done
        if [ -z "$found" ]; then
            echo "warning: could not locate dependency $name" >&2
            continue
        fi
        cp -Lu "$found" "$OUTDIR/lib/"
        queue+=("$found")
    done
done

echo "Built $OUTDIR/procwatch (self-contained; runtime libs bundled in $OUTDIR/lib/)"
