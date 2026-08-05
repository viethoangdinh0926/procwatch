#!/bin/bash
# Downloads the per-runtime agent payloads that the injector points processes
# at, into build/agent-runtime/. Kept separate from compilation because these
# are large, network-fetched, and rarely change: run once, then build as often
# as you like.
#
# Usage: fetch_runtimes.sh [output_dir]
#
# Environment:
#   JAVAAGENT_VERSION   OpenTelemetry Java agent release (default: latest)
#   PYTHON_VERSIONS     Space-separated CPython versions (default: "3.12")
#   SKIP_PYTHON_MUSL    Set to 1 to skip building the Alpine/musl trees
set -e

OUTDIR="${1:-build/agent-runtime}"
JAVAAGENT_VERSION="${JAVAAGENT_VERSION:-latest}"
PYTHON_VERSIONS="${PYTHON_VERSIONS:-3.12}"
DOCKER="${DOCKER:-docker}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# `make agent` runs in containers as root, which can leave build/ root-owned
# and this mkdir failing. Repair it in a throwaway root container rather than
# demanding sudo from the caller.
if [ -e build ] && [ ! -w build ]; then
    echo "build/ is not writable; fixing ownership"
    $DOCKER run --rm -v "$(pwd):/work" -w /work alpine:3.20 \
        chown -R "$HOST_UID:$HOST_GID" build
fi

mkdir -p "$OUTDIR/java" "$OUTDIR/python"

# ------------------------ Java ------------------------

if [ "$JAVAAGENT_VERSION" = "latest" ]; then
    JAR_URL="https://github.com/open-telemetry/opentelemetry-java-instrumentation/releases/latest/download/opentelemetry-javaagent.jar"
else
    JAR_URL="https://github.com/open-telemetry/opentelemetry-java-instrumentation/releases/download/v${JAVAAGENT_VERSION}/opentelemetry-javaagent.jar"
fi

echo "Fetching OpenTelemetry Java agent ($JAVAAGENT_VERSION)"
curl -fsSL "$JAR_URL" -o "$OUTDIR/java/javaagent.jar.tmp"

# A truncated or HTML error page saved as javaagent.jar would abort JVM
# startup for every injected application, so verify it is really a zip.
if ! head -c 2 "$OUTDIR/java/javaagent.jar.tmp" | grep -q 'PK'; then
    echo "error: downloaded javaagent.jar is not a zip archive" >&2
    rm -f "$OUTDIR/java/javaagent.jar.tmp"
    exit 1
fi
mv "$OUTDIR/java/javaagent.jar.tmp" "$OUTDIR/java/javaagent.jar"
chmod 0644 "$OUTDIR/java/javaagent.jar"
echo "  -> $OUTDIR/java/javaagent.jar ($(du -h "$OUTDIR/java/javaagent.jar" | cut -f1))"

# ------------------------ Python ------------------------

# The shim in runtime/python/sitecustomize.py is version-agnostic and lives
# alongside the per-ABI trees it dispatches to.
cp runtime/python/sitecustomize.py "$OUTDIR/python/sitecustomize.py"
chmod 0644 "$OUTDIR/python/sitecustomize.py"

PY_CORE="opentelemetry-distro opentelemetry-exporter-otlp-proto-http opentelemetry-instrumentation"

# An explicit list rather than `opentelemetry-bootstrap`, which only installs
# instrumentation for libraries it can already see. The payload is built in a
# bare interpreter image with no application present, so bootstrap detects
# nothing and silently produces a tree that initialises cleanly and then emits
# no spans.
#
# Installing these is safe and cheap: an instrumentation package depends on
# the OTel API, not on the library it targets, and each one no-ops unless its
# target is importable in the process it lands in.
#
# grpc is deliberately absent: it would pull grpcio, a large wheel with a
# compiled extension, and the exporter here speaks http/protobuf anyway.
PY_INSTRUMENTATION="${PY_INSTRUMENTATION:-\
opentelemetry-instrumentation-urllib \
opentelemetry-instrumentation-urllib3 \
opentelemetry-instrumentation-requests \
opentelemetry-instrumentation-httpx \
opentelemetry-instrumentation-aiohttp-client \
opentelemetry-instrumentation-wsgi \
opentelemetry-instrumentation-asgi \
opentelemetry-instrumentation-flask \
opentelemetry-instrumentation-django \
opentelemetry-instrumentation-fastapi \
opentelemetry-instrumentation-starlette \
opentelemetry-instrumentation-tornado \
opentelemetry-instrumentation-pyramid \
opentelemetry-instrumentation-falcon \
opentelemetry-instrumentation-psycopg2 \
opentelemetry-instrumentation-pymysql \
opentelemetry-instrumentation-mysql \
opentelemetry-instrumentation-sqlalchemy \
opentelemetry-instrumentation-sqlite3 \
opentelemetry-instrumentation-redis \
opentelemetry-instrumentation-pymongo \
opentelemetry-instrumentation-elasticsearch \
opentelemetry-instrumentation-celery \
opentelemetry-instrumentation-pika \
opentelemetry-instrumentation-kafka-python \
opentelemetry-instrumentation-boto3sqs \
opentelemetry-instrumentation-jinja2 \
opentelemetry-instrumentation-logging \
opentelemetry-instrumentation-threading \
opentelemetry-instrumentation-asyncio}"

fetch_python_tree() {
    local image="$1" tag="$2"
    local target="$OUTDIR/python/$tag"

    echo "Building Python tree $tag from $image"
    # Must start empty. pip --target refuses to replace directories that are
    # already present, so a leftover tree would cause packages to be skipped.
    rm -rf "$target"
    mkdir -p "$target"

    # Built inside the matching interpreter image so pip resolves wheels for
    # that exact CPython version and libc. Installing from the host would
    # produce a tree that fails with "symbol not found" on first import.
    #
    # The package lists are passed through the environment and the script body
    # is single-quoted, so nothing is expanded or escaped by the host shell.
    # Everything in one pip invocation, which matters for more than speed.
    # `pip install --target` skips any top-level directory that already
    # exists, warning "Target directory already exists. Specify --upgrade to
    # force replacement." Installing package by package therefore moves each
    # one's .dist-info into place but silently discards its opentelemetry/
    # subtree, producing a tree whose entry points resolve and whose modules
    # do not import. Resolving the set together also keeps the instrumentation
    # packages, which are released in lockstep, on matching versions.
    $DOCKER run --rm \
        -e PW_PKGS="$PY_CORE $PY_INSTRUMENTATION" \
        -e PW_UID="$HOST_UID" -e PW_GID="$HOST_GID" \
        -v "$(pwd)/$target:/out" "$image" /bin/sh -c '
        set -e
        pip install --quiet --upgrade pip --trusted-host pypi.org --trusted-host files.pythonhosted.org >/dev/null 2>&1 || true
        pip install --quiet --target /out --trusted-host pypi.org --trusted-host files.pythonhosted.org $PW_PKGS
        chmod -R go+rX /out
        chown -R "$PW_UID:$PW_GID" /out
    '

    # Count actual module directories, not .dist-info: the failure above
    # leaves the dist-info behind and would pass a metadata-only check while
    # emitting no spans at runtime.
    local found
    found=$(find "$target/opentelemetry/instrumentation" -maxdepth 1 -mindepth 1 \
            -type d ! -name '__pycache__' 2>/dev/null | wc -l)
    if [ "$found" -lt 10 ]; then
        echo "error: $tag has only $found instrumentation modules; expected 10+" >&2
        echo "       such a tree initialises cleanly at runtime and emits no spans" >&2
        exit 1
    fi
    echo "  -> $target ($(du -sh "$target" | cut -f1), $found instrumentation modules)"
}

for version in $PYTHON_VERSIONS; do
    short="cp${version//./}"
    fetch_python_tree "python:${version}-slim" "$short"
    if [ "${SKIP_PYTHON_MUSL:-0}" != "1" ]; then
        # Alpine needs its own tree: a glibc-built extension loaded under musl
        # fails to relocate, and musl treats that as fatal.
        fetch_python_tree "python:${version}-alpine" "${short}-musl"
    fi
done

echo
echo "Runtime payloads ready in $OUTDIR"
echo "  java/javaagent.jar"
echo "  python/sitecustomize.py + $(ls -d "$OUTDIR"/python/cp* 2>/dev/null | wc -l) ABI tree(s)"
