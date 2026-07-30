BUILD_DIR := build
x86_64_APP := $(BUILD_DIR)/x86_64_procwatch
aarch64_APP := $(BUILD_DIR)/aarch64_procwatch

SRCS := src/main.c src/util.c src/proc_scan.c src/db.c

CC_X86   ?= gcc
CC_AARCH64  ?= aarch64-linux-gnu-gcc

CFLAGS_COMMON ?= -O2 -Wall -Wextra -Iinclude
DOCKER ?= docker
DOCKER_AARCH64_IMAGE ?= dockcross/linux-arm64:latest
DOCKER_X86_IMAGE ?= dockcross/linux-x64:latest

.PHONY: all x86_64 aarch64 clean

all: x86_64 aarch64

x86_64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_X86_IMAGE) \
	  /bin/bash -lc "set -e; apt-get update && apt-get install -y libpq-dev libssl-dev zlib1g-dev \
	  && $(CC_X86) $(CFLAGS_COMMON) -I/usr/include/postgresql $(SRCS) -lpq -lssl -lcrypto -lz -pthread -o $(x86_64_APP)"

aarch64:
	$(DOCKER) run --rm -v $(CURDIR):/work -w /work $(DOCKER_AARCH64_IMAGE) \
	  /bin/bash -lc "set -e; apt-get update && dpkg --add-architecture arm64 && apt-get update \
	  && apt-get install -y gcc-aarch64-linux-gnu libpq-dev:arm64 libssl-dev:arm64 zlib1g-dev:arm64 \
	  && $(CC_AARCH64) $(CFLAGS_COMMON) -I/usr/include/postgresql $(SRCS) -lpq -lssl -lcrypto -lz -pthread -o $(aarch64_APP)"

clean:
	rm -f $(x86_64_APP) $(aarch64_APP)
