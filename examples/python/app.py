"""Minimal Python workload for the demo stack.

As with the Java sample, there is no OpenTelemetry import here. The injector
puts the agent's sitecustomize on PYTHONPATH, and CPython's `site` module
imports it during startup, before this file runs.

Flask rather than http.server because the stdlib HTTP server has no
OpenTelemetry instrumentation, so it would produce no server spans and make
a working injection look broken. The app also calls itself on a timer, which
the urllib instrumentation turns into client spans.

On startup the parent also spawns child `python ... --worker` processes so
inject process-tree metrics cover nested interpreters under the same label.
"""

import os
import random
import subprocess
import sys
import threading
import time
import urllib.request

from flask import Flask, jsonify

PORT = int(os.environ.get("PORT", "8080"))


def worker_count():
    raw = os.environ.get("PROCWATCH_DEMO_WORKERS", "2")
    try:
        return max(0, int(raw))
    except ValueError:
        return 2


def run_worker(worker_id):
    """Child interpreter: light CPU + retained memory for process metrics."""
    print("python worker-%s pid=%d" % (worker_id, os.getpid()), flush=True)
    retained = []
    scratch = 0
    while True:
        end = time.time() + 0.05
        while time.time() < end:
            scratch += 1
        if len(retained) < 8:
            retained.append(bytearray(256 * 1024))
        time.sleep(0.5)
        if scratch == 0:
            print("python worker-%s alive" % worker_id, flush=True)


def start_workers(count):
    children = []
    for i in range(count):
        child = subprocess.Popen(
            [sys.executable, __file__, "--worker", str(i)],
            stdout=sys.stdout,
            stderr=sys.stderr,
        )
        children.append(child)
        print("spawned python worker-%d pid=%d" % (i, child.pid), flush=True)
    return children


app = Flask(__name__)


@app.route("/health")
def health():
    return "ok"


@app.route("/pay")
def pay():
    time.sleep(random.uniform(0.02, 0.08))
    if random.random() < 0.2:
        # Gives the demo some ERROR spans to query.
        return jsonify(error="downstream unavailable"), 500
    return jsonify(payment="captured")


def generate_traffic():
    time.sleep(3)
    while True:
        try:
            with urllib.request.urlopen(
                "http://localhost:%d/pay" % PORT, timeout=2
            ) as response:
                print("self-call -> %d" % response.status, flush=True)
        except Exception as exc:  # noqa: BLE001 - demo traffic, keep going
            print("self-call failed: %r" % exc, flush=True)
        time.sleep(5)


if __name__ == "__main__":
    if "--worker" in sys.argv:
        idx = sys.argv.index("--worker")
        wid = sys.argv[idx + 1] if idx + 1 < len(sys.argv) else "0"
        run_worker(wid)
        raise SystemExit(0)

    children = start_workers(worker_count())
    threading.Thread(target=generate_traffic, daemon=True).start()
    print("payments listening on %d" % PORT, flush=True)
    try:
        # The reloader would fork a second interpreter and double the spans.
        app.run(host="0.0.0.0", port=PORT, threaded=True, use_reloader=False)
    finally:
        for child in children:
            child.terminate()
