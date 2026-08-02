"""Minimal Python workload for the demo stack.

As with the Java sample, there is no OpenTelemetry import here. The injector
puts the agent's sitecustomize on PYTHONPATH, and CPython's `site` module
imports it during startup, before this file runs.

Flask rather than http.server because the stdlib HTTP server has no
OpenTelemetry instrumentation, so it would produce no server spans and make
a working injection look broken. The app also calls itself on a timer, which
the urllib instrumentation turns into client spans.
"""

import random
import threading
import time
import urllib.request

from flask import Flask, jsonify

PORT = 8080

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
    threading.Thread(target=generate_traffic, daemon=True).start()
    print("payments listening on %d" % PORT, flush=True)
    # The reloader would fork a second interpreter and double the spans.
    app.run(host="0.0.0.0", port=PORT, threaded=True, use_reloader=False)
