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

# Each target produces a self-contained bundle: <arch>/procwatch plus its
# runtime shared libs (libpq and everything it transitively depends on) in
# <arch>/lib/, so the binary runs on hosts that don't have libpq installed.
# See scripts/build.sh for details.

.PHONY: all x86_64 aarch64 clean

all: x86_64 aarch64

x86_64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_X86_IMAGE) \
	  /bin/bash scripts/build.sh x86_64 $(CC_X86) $(CFLAGS_COMMON)

aarch64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_AARCH64_IMAGE) \
	  /bin/bash scripts/build.sh aarch64 $(CC_AARCH64) $(CFLAGS_COMMON)

clean:
	rm -rf $(x86_64_DIR) $(aarch64_DIR)
