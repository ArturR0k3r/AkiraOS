# AkiraOS WASM Sample Apps - Master Makefile
#
# Build all sample WASM apps for AkiraOS
#
# Usage:
#   make          - Build all samples
#   make clean    - Clean all samples
#   make install  - Install to SD card (if mounted)
#   make check    - Verify WASI SDK installation
#
# Requirements:
#   - WASI SDK installed (see docs/APP_DEVELOPMENT.md)
#   - Set WASI_SDK environment variable or install to /opt/wasi-sdk

# WASI SDK path (override with: make WASI_SDK=/path/to/wasi-sdk)
WASI_SDK ?= /opt/wasi-sdk

# Sample directories
SAMPLES = hello_world sensor_demo blink_led

# SD card mount point for install target
SD_MOUNT ?= /media/$(USER)/AKIRA

# Colors
GREEN = \033[0;32m
RED = \033[0;31m
YELLOW = \033[1;33m
NC = \033[0m

.PHONY: all clean install check help $(SAMPLES)

all: check $(SAMPLES)
	@echo ""
	@echo -e "$(GREEN)✓ All samples built successfully!$(NC)"
	@echo ""
	@echo "Built WASM files:"
	@find . -name "*.wasm" -type f | while read f; do \
		size=$$(ls -la "$$f" | awk '{print $$5}'); \
		echo "  $$f ($$size bytes)"; \
	done

$(SAMPLES):
	@echo -e "$(GREEN)Building $@...$(NC)"
	@$(MAKE) -C $@ WASI_SDK=$(WASI_SDK) --no-print-directory

clean:
	@echo "Cleaning all samples..."
	@for dir in $(SAMPLES); do \
		$(MAKE) -C $$dir clean --no-print-directory; \
	done
	@echo -e "$(GREEN)✓ Clean complete$(NC)"

check:
	@echo "Checking WASI SDK..."
	@if [ ! -d "$(WASI_SDK)" ]; then \
		echo -e "$(RED)Error: WASI SDK not found at $(WASI_SDK)$(NC)"; \
		echo ""; \
		echo "Please install WASI SDK:"; \
		echo "  1. Download from: https://github.com/WebAssembly/wasi-sdk/releases"; \
		echo "  2. Extract to /opt/wasi-sdk"; \
		echo "  3. Or set WASI_SDK=/path/to/wasi-sdk"; \
		echo ""; \
		exit 1; \
	fi
	@if [ ! -x "$(WASI_SDK)/bin/clang" ]; then \
		echo -e "$(RED)Error: clang not found in $(WASI_SDK)/bin$(NC)"; \
		exit 1; \
	fi
	@echo -e "$(GREEN)✓ WASI SDK OK: $(WASI_SDK)$(NC)"
	@$(WASI_SDK)/bin/clang --version | head -1

install: all
	@echo ""
	@echo "Installing to SD card..."
	@if [ ! -d "$(SD_MOUNT)" ]; then \
		echo -e "$(YELLOW)Warning: SD card not mounted at $(SD_MOUNT)$(NC)"; \
		echo "Mount your SD card or set SD_MOUNT=/path/to/mount"; \
		exit 1; \
	fi
	@mkdir -p "$(SD_MOUNT)/apps"
	@for dir in $(SAMPLES); do \
		if [ -f "$$dir/$$dir.wasm" ]; then \
			cp "$$dir/$$dir.wasm" "$(SD_MOUNT)/apps/"; \
			echo "  Copied $$dir.wasm"; \
		fi; \
		if [ -f "$$dir/manifest.json" ]; then \
			cp "$$dir/manifest.json" "$(SD_MOUNT)/apps/$$dir.manifest.json"; \
			echo "  Copied $$dir.manifest.json"; \
		fi; \
	done
	@sync
	@echo -e "$(GREEN)✓ Apps installed to $(SD_MOUNT)/apps$(NC)"

info:
	@echo "=== AkiraOS WASM Samples ==="
	@echo ""
	@echo "Samples:"
	@for dir in $(SAMPLES); do \
		echo "  - $$dir"; \
	done
	@echo ""
	@echo "WASI SDK: $(WASI_SDK)"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build all samples"
	@echo "  make <name>   - Build single sample"
	@echo "  make clean    - Clean all"
	@echo "  make install  - Copy to SD card"
	@echo "  make check    - Verify toolchain"

help: info
