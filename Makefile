BUILD_DIR := build
x86_64_DIR := $(BUILD_DIR)/x86_64
aarch64_DIR := $(BUILD_DIR)/aarch64
x86_64_APP := $(x86_64_DIR)/procwatch
aarch64_APP := $(aarch64_DIR)/procwatch

CC_X86   ?= gcc
CC_AARCH64  ?= aarch64-linux-gnu-gcc

CFLAGS_COMMON ?= -O2 -Wall -Wextra -Iinclude
DOCKER ?= docker
DOCKER_AARCH64_IMAGE ?= dockcross/linux-arm64:latest
DOCKER_X86_IMAGE ?= dockcross/linux-x64:latest

AGENT_DIR := $(BUILD_DIR)/agent
AGENT_RUNTIME_DIR := $(BUILD_DIR)/agent-runtime

# Each target produces a self-contained bundle: <arch>/procwatch plus its
# runtime shared libs (libpq and everything it transitively depends on) in
# <arch>/lib/, so the binary runs on hosts that don't have libpq installed.
# See scripts/build.sh for details.

.PHONY: all x86_64 aarch64 clean \
        agent agent-x86_64 agent-aarch64 agent-musl-x86_64 agent-musl-aarch64 \
        runtimes world clean-agent clean-all

all: x86_64 aarch64

x86_64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_X86_IMAGE) \
	  /bin/bash scripts/build.sh x86_64 $(CC_X86) $(CFLAGS_COMMON)

aarch64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_AARCH64_IMAGE) \
	  /bin/bash scripts/build.sh aarch64 $(CC_AARCH64) $(CFLAGS_COMMON)

clean:
	rm -rf $(x86_64_DIR) $(aarch64_DIR)

# ------------------------ Auto-instrumentation agent ------------------------
#
# Separate targets and a separate build script (scripts/build_agent.sh) so the
# procwatch binary above is unaffected. `make all` still builds only procwatch;
# use `make world` for both.
#
# Bundle layout, mirroring what the init container copies into a pod:
#   build/agent/<arch>/procwatch-agentd
#   build/agent/<arch>/lib/libprocwatch_inject.so   <- the LD_PRELOAD library
#   build/agent/<arch>/lib/*.so                     <- bundled libpq et al
#   build/agent/<arch>/java/javaagent.jar
#   build/agent/<arch>/python/sitecustomize.py + cp3XX trees

agent: agent-x86_64 agent-aarch64 agent-musl-x86_64

world: all agent

# Downloads the OpenTelemetry Java agent and builds the per-ABI Python trees.
# Run this before `make agent` if you want code-level tracing; without it the
# bundle still builds and collects process metrics.
runtimes:
	/bin/bash scripts/fetch_runtimes.sh $(AGENT_RUNTIME_DIR)

# HOST_UID/HOST_GID are passed through so the container can hand the build
# tree back to the invoking user; otherwise build/ ends up root-owned and
# later host-side steps fail on permissions.
HOST_UID := $(shell id -u)
HOST_GID := $(shell id -g)
DOCKER_AS_USER := -e HOST_UID=$(HOST_UID) -e HOST_GID=$(HOST_GID)

agent-x86_64:
	$(DOCKER) run --rm $(DOCKER_AS_USER) -v $(CURDIR):/work -w /work $(DOCKER_X86_IMAGE) \
	  /bin/bash scripts/build_agent.sh x86_64 $(CC_X86) $(CFLAGS_COMMON)

agent-aarch64:
	$(DOCKER) run --rm $(DOCKER_AS_USER) -v $(CURDIR):/work -w /work $(DOCKER_AARCH64_IMAGE) \
	  /bin/bash scripts/build_agent.sh aarch64 $(CC_AARCH64) $(CFLAGS_COMMON)

# Alpine images need a musl-linked injector. The glibc build is not a usable
# fallback there: it names libc.so.6 as a dependency, which musl cannot
# satisfy, so the loader ignores it and the workload runs uninstrumented with
# no error anywhere.
agent-musl-x86_64:
	/bin/bash scripts/build_inject_musl.sh x86_64

agent-musl-aarch64:
	/bin/bash scripts/build_inject_musl.sh aarch64

clean-agent:
	rm -rf $(AGENT_DIR)

clean-all: clean clean-agent
	rm -rf $(AGENT_RUNTIME_DIR)
