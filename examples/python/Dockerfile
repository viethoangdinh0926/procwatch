# Stock Python image with the application's own dependency (Flask) but no
# OpenTelemetry packages. The agent tree is mounted at runtime and reached via
# PYTHONPATH, so instrumentation arrives entirely from outside the image.
#
# The tag here must match a cp3XX tree built by scripts/fetch_runtimes.sh.
# 3.12 is the default; for anything else run:
#   PYTHON_VERSIONS="3.11" make runtimes
FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir flask==3.0.3
COPY python/app.py .
CMD ["python", "app.py"]
