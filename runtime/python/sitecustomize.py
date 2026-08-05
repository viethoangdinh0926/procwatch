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

import gc
import os
import sys
import time

_DEBUG = bool(os.environ.get("PROCWATCH_INJECT_DEBUG"))
_SELF_DIR = os.path.dirname(os.path.abspath(__file__))
_START_TIME = time.time()


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


def _proc_status_fields():
    """(rss_bytes, vms_bytes, threads) from /proc/self/status, Linux only.

    resource.getrusage(RUSAGE_SELF).ru_maxrss is a peak, not current, RSS, and
    has no thread count or VMS at all, so it cannot stand in for this.
    """
    rss = vms = threads = 0
    try:
        with open("/proc/self/status", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1]) * 1024
                elif line.startswith("VmSize:"):
                    vms = int(line.split()[1]) * 1024
                elif line.startswith("Threads:"):
                    threads = int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return rss, vms, threads


def _open_fd_count():
    try:
        return len(os.listdir("/proc/self/fd"))
    except OSError:
        return 0


def _cpu_seconds():
    # RUSAGE_SELF: this process's own threads, not children (procwatch-wrap
    # and the inject metric thread are likewise per-process, not per-tree).
    import resource

    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime + usage.ru_stime


def _setup_process_metrics():
    """Observable instruments matching procwatch's <label>_procs columns
    (cpu_pct, rss_kb-equivalent, threads), plus extras procwatch's /proc
    sampling does not cover (VMS, open fds, GC activity, interpreter info).
    These ride the same OTLP metrics exporter/interval the auto-instrumented
    SDK already has configured, landing in agentd's <label>_otel_metrics.
    """
    from opentelemetry import metrics
    from opentelemetry.metrics import CallbackOptions, Observation

    meter = metrics.get_meter("procwatch.process", "1.0.0")

    # cpu.percent mirrors procwatch's cpu_pct: 100 * cpu-seconds consumed per
    # wall-clock second since the previous sample, so a process fully busy on
    # two cores reads ~200, matching "top"-style percentages rather than a
    # 0..1 utilization ratio.
    _cpu_prev = {"cpu": _cpu_seconds(), "wall": time.time()}

    def observe_cpu_percent(_options):
        now_cpu = _cpu_seconds()
        now_wall = time.time()
        dt = now_wall - _cpu_prev["wall"]
        pct = 0.0
        if dt > 0:
            pct = 100.0 * (now_cpu - _cpu_prev["cpu"]) / dt
        _cpu_prev["cpu"] = now_cpu
        _cpu_prev["wall"] = now_wall
        yield Observation(pct)

    def observe_rss(_options):
        rss, _vms, _threads = _proc_status_fields()
        yield Observation(rss)

    def observe_vms(_options):
        _rss, vms, _threads = _proc_status_fields()
        yield Observation(vms)

    def observe_threads(_options):
        _rss, _vms, threads = _proc_status_fields()
        yield Observation(threads)

    def observe_open_fds(_options):
        yield Observation(_open_fd_count())

    def observe_start_time(_options):
        yield Observation(_START_TIME)

    def observe_gc_collections(_options):
        for gen, st in enumerate(gc.get_stats()):
            yield Observation(st.get("collections", 0), {"generation": str(gen)})

    def observe_gc_collected(_options):
        for gen, st in enumerate(gc.get_stats()):
            yield Observation(st.get("collected", 0), {"generation": str(gen)})

    def observe_gc_uncollectable(_options):
        for gen, st in enumerate(gc.get_stats()):
            yield Observation(st.get("uncollectable", 0), {"generation": str(gen)})

    def observe_python_info(_options):
        import platform

        yield Observation(
            1,
            {
                "version": platform.python_version(),
                "implementation": platform.python_implementation(),
            },
        )

    meter.create_observable_gauge(
        "process.cpu.percent",
        callbacks=[observe_cpu_percent],
        description="CPU used, as a percentage of one core, since the previous sample",
        unit="%",
    )
    meter.create_observable_gauge(
        "process.resident_memory.bytes",
        callbacks=[observe_rss],
        description="Resident memory size (RSS)",
        unit="By",
    )
    meter.create_observable_gauge(
        "process.virtual_memory.bytes",
        callbacks=[observe_vms],
        description="Virtual memory size (VMS)",
        unit="By",
    )
    meter.create_observable_gauge(
        "process.threads",
        callbacks=[observe_threads],
        description="Number of OS threads",
    )
    meter.create_observable_gauge(
        "process.open_fds",
        callbacks=[observe_open_fds],
        description="Number of open file descriptors",
    )
    meter.create_observable_gauge(
        "process.start_time.seconds",
        callbacks=[observe_start_time],
        description="Start time of the process since the Unix epoch",
        unit="s",
    )
    meter.create_observable_counter(
        "python.gc.collections",
        callbacks=[observe_gc_collections],
        description="Number of times this generation was collected",
    )
    meter.create_observable_counter(
        "python.gc.objects.collected",
        callbacks=[observe_gc_collected],
        description="Objects collected by generation",
    )
    meter.create_observable_counter(
        "python.gc.objects.uncollectable",
        callbacks=[observe_gc_uncollectable],
        description="Uncollectable objects by generation",
    )
    meter.create_observable_gauge(
        "python.info",
        callbacks=[observe_python_info],
        description="Python interpreter version/implementation",
    )


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
        _setup_process_metrics()
        _log("process metrics instruments registered")
    except Exception as exc:  # noqa: BLE001 - never break the app
        _log("process metrics setup failed: %r" % exc)


try:
    _bootstrap()
except Exception as exc:  # noqa: BLE001 - absolute last resort
    _log("bootstrap crashed: %r" % exc)

try:
    _chain_to_shadowed_sitecustomize()
except Exception as exc:  # noqa: BLE001
    _log("chaining crashed: %r" % exc)
