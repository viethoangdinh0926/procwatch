"""ProcWatch Python auto-instrumentation shim.

The injector puts this file's directory at the front of PYTHONPATH. CPython
imports `site` during startup, and `site` imports `sitecustomize` if it can
find one, which is how we get control before the application's own code runs.

This file must stay pure Python and version-agnostic, because it is loaded by
whatever interpreter the application happens to use. The actual OpenTelemetry
packages cannot be: wheels such as grpcio carry compiled extensions bound to
one CPython version and one libc, and importing a mismatched tree fails with
"symbol not found". So the vendored packages live in per-ABI subdirectories
and this shim picks the matching one at runtime:

    <agent>/python/sitecustomize.py   <- this file, on PYTHONPATH
    <agent>/python/cp312/             <- glibc CPython 3.12 packages
    <agent>/python/cp312-musl/        <- musl CPython 3.12 packages

Every failure path here is silent by default. An observability agent that
prevents an application from starting is worse than no agent at all; set
PROCWATCH_INJECT_DEBUG=1 to see what happened.
"""

import os
import sys

_DEBUG = bool(os.environ.get("PROCWATCH_INJECT_DEBUG"))
_SELF_DIR = os.path.dirname(os.path.abspath(__file__))


def _log(message):
    if _DEBUG:
        sys.stderr.write("procwatch-python: %s\n" % message)


def _is_musl():
    # platform.libc_ver() reports an empty string on musl rather than naming
    # it, so probe for the loader directly.
    try:
        import glob

        if glob.glob("/lib/ld-musl-*.so.1"):
            return True
    except Exception:
        pass
    return False


def _candidate_dirs():
    tag = "cp%d%d" % (sys.version_info[0], sys.version_info[1])
    if _is_musl():
        # Only the musl tree is viable here; loading the glibc one would
        # raise ImportError on the first compiled extension.
        return ["%s-musl" % tag]
    return [tag]


def _find_package_tree():
    for name in _candidate_dirs():
        path = os.path.join(_SELF_DIR, name)
        if os.path.isdir(path):
            return path
    return None


def _strip_self_from_pythonpath():
    """Remove our directory from PYTHONPATH for the duration of bootstrap.

    Loading instrumentors can spawn subprocesses. If those inherit a
    PYTHONPATH that still points here, they re-enter this shim while we are
    part-way through initialising, which upstream describes as a recursive
    loop. Mirrors what opentelemetry's own initialize() does.
    """
    current = os.environ.get("PYTHONPATH")
    if current is None:
        return None, False
    parts = current.split(os.pathsep)
    kept = [p for p in parts if p and os.path.abspath(p) != _SELF_DIR]
    if len(kept) == len(parts):
        return current, False
    os.environ["PYTHONPATH"] = os.pathsep.join(kept)
    return current, True


def _chain_to_shadowed_sitecustomize():
    """Run any sitecustomize we displaced.

    Because our directory is prepended to PYTHONPATH, `site` finds this file
    instead of one the application or image may already ship. Silently
    dropping theirs is a real breakage, so locate the next candidate and
    execute it.
    """
    for entry in sys.path:
        if not entry or os.path.abspath(entry) == _SELF_DIR:
            continue
        candidate = os.path.join(entry, "sitecustomize.py")
        if not os.path.isfile(candidate):
            continue
        try:
            with open(candidate, "r") as handle:
                source = handle.read()
            namespace = {"__file__": candidate, "__name__": "sitecustomize"}
            exec(compile(source, candidate, "exec"), namespace)
            _log("chained to %s" % candidate)
        except Exception as exc:  # noqa: BLE001 - never break the app
            _log("chained sitecustomize failed: %r" % exc)
        return


def _bootstrap():
    if os.environ.get("PROCWATCH_INJECT_DISABLED", "") not in ("", "0"):
        _log("disabled by PROCWATCH_INJECT_DISABLED")
        return

    tree = _find_package_tree()
    if tree is None:
        _log(
            "no vendored packages for CPython %d.%d (%s); "
            "run scripts/fetch_runtimes.sh for this version"
            % (sys.version_info[0], sys.version_info[1],
               "musl" if _is_musl() else "glibc")
        )
        return

    if tree not in sys.path:
        # Appended, not prepended: the application's own dependencies must
        # win any name collision with the agent's.
        sys.path.append(tree)

    original, changed = _strip_self_from_pythonpath()
    try:
        from opentelemetry.instrumentation.auto_instrumentation import (  # noqa: E501
            initialize,
        )

        initialize()
        _log("instrumentation initialised from %s" % tree)
    except Exception as exc:  # noqa: BLE001 - never break the app
        _log("initialisation failed: %r" % exc)
    finally:
        if changed and original is not None:
            os.environ["PYTHONPATH"] = original


try:
    _bootstrap()
except Exception as exc:  # noqa: BLE001 - absolute last resort
    _log("bootstrap crashed: %r" % exc)

try:
    _chain_to_shadowed_sitecustomize()
except Exception as exc:  # noqa: BLE001
    _log("chaining crashed: %r" % exc)
