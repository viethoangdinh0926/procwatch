#!/bin/bash
# Builds the auto-instrumentation agent: procwatch-agentd plus the
# LD_PRELOAD injector library, packaged as a self-contained bundle at
# build/agent/<arch>/.
#
# Deliberately a sibling of scripts/build.sh rather than an extension of it,
# so the procwatch binary's build path carries no regression risk from
# changes here.
#
# Usage: build_agent.sh <arch: x86_64|aarch64> <cc> [cflags...]
set -e

ARCH="$1"
CC="$2"
shift 2
CFLAGS=("$@")

AGENT_SRCS=(
    src/agent/agentd.c
    src/agent/http_server.c
    src/agent/pb_decode.c
    src/agent/otlp_trace.c
    src/agent/db_otlp.c
    src/agent/collector.c
    src/util.c
    src/proc_scan.c
    src/db.c
)
INJECT_SRCS=(src/inject/inject.c src/inject/detect.c src/inject/envmod.c)

OUTDIR="build/agent/$ARCH"

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

mkdir -p "$OUTDIR/lib" "$OUTDIR/java" "$OUTDIR/python"

# The injector is loaded into every process on the node, so it is built to a
# stricter standard than the daemon:
#   --no-undefined     a preloaded object that loads but fails to resolve a
#                      symbol aborts the process on both glibc and musl, so
#                      every symbol must be resolved at link time
#   -fvisibility=hidden plus the version script: export nothing, or we would
#                      interpose libc functions process-wide by accident
#   no -lpq, no -lz    libc must remain the only DT_NEEDED, minimising what
#                      could fail to resolve at load time
"$CC" "${CFLAGS[@]}" -fPIC -shared -fvisibility=hidden \
    "${INJECT_SRCS[@]}" \
    -Wl,--version-script=src/inject/inject.map \
    -Wl,--no-undefined -Wl,-z,now -Wl,-z,relro \
    -o "$OUTDIR/lib/libprocwatch_inject.so"

needed=$(readelf -d "$OUTDIR/lib/libprocwatch_inject.so" |
         sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' |
         grep -v '^libc\.so' || true)
if [ -n "$needed" ]; then
    echo "error: injector must depend only on libc, but also needs: $needed" >&2
    exit 1
fi
if nm -D --defined-only "$OUTDIR/lib/libprocwatch_inject.so" | grep -q .; then
    echo "error: injector exports symbols; it must export none" >&2
    exit 1
fi

# Same $ORIGIN/lib rpath trick as scripts/build.sh: classic DT_RPATH via
# --disable-new-dtags so it propagates to libpq's own transitive
# dependencies, which DT_RUNPATH would not do.
"$CC" "${CFLAGS[@]}" -I/usr/include/postgresql "${AGENT_SRCS[@]}" \
    -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/lib' \
    -lpq -lssl -lcrypto -lz -pthread \
    -o "$OUTDIR/procwatch-agentd"

# Bundle the transitive closure of shared libs, excluding libc/ld-linux which
# the base OS always provides. readelf rather than ldd because aarch64 is
# cross-compiled on an x86_64 host, where ldd cannot run a foreign binary.
declare -A seen=()
queue=("$OUTDIR/procwatch-agentd")
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

# Runtime payloads, if scripts/fetch_runtimes.sh has been run. The injector
# checks for these at startup and no-ops when absent, so a bundle without
# them is still valid, it just does metrics only.
if [ -d build/agent-runtime ]; then
    # Replaced rather than merged: a stale tree from a previous fetch would
    # otherwise leave orphaned modules and .dist-info metadata behind, and
    # mismatched metadata makes Python's entry point discovery advertise
    # instrumentation that is no longer present.
    rm -rf "$OUTDIR/java" "$OUTDIR/python"
    mkdir -p "$OUTDIR/java" "$OUTDIR/python"
    cp -a build/agent-runtime/java/. "$OUTDIR/java/" 2>/dev/null || true
    cp -a build/agent-runtime/python/. "$OUTDIR/python/" 2>/dev/null || true
fi

# The dockcross container runs as root, so without this the whole build tree
# ends up root-owned and subsequent host-side steps (make runtimes, docker
# build, make clean) fail with permission errors.
if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
    chown -R "$HOST_UID:$HOST_GID" build 2>/dev/null || true
fi

echo "Built $OUTDIR/procwatch-agentd and $OUTDIR/lib/libprocwatch_inject.so"
if [ ! -f "$OUTDIR/java/javaagent.jar" ]; then
    echo "note: no java/javaagent.jar - run scripts/fetch_runtimes.sh for Java tracing"
fi
if [ ! -f "$OUTDIR/python/sitecustomize.py" ]; then
    echo "note: no python/sitecustomize.py - run scripts/fetch_runtimes.sh for Python tracing"
fi
