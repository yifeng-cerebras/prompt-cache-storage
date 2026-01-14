CMAKE ?= cmake
ROOT_DIR := $(abspath .)
BUILD_DIR ?= $(ROOT_DIR)/build
CMAKE_BUILD_TYPE ?= Release
CMAKE_PREFIX_PATH ?=

# Optional: set TOOLCHAIN_FILE or VCPKG_ROOT for vcpkg builds.
ifdef TOOLCHAIN_FILE
CMAKE_TOOLCHAIN := -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)
else ifdef VCPKG_ROOT
CMAKE_TOOLCHAIN := -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
endif

ifneq ($(strip $(CMAKE_PREFIX_PATH)),)
CMAKE_PREFIX_OPT := -DCMAKE_PREFIX_PATH=$(CMAKE_PREFIX_PATH)
endif

.PHONY: all build configure test clean

all: build

configure:
	@if [ -z "$(CMAKE_PREFIX_PATH)" ]; then \
		echo "Set CMAKE_PREFIX_PATH to a RocksDB install prefix."; \
		echo "Example: make build CMAKE_PREFIX_PATH=/path/to/rocksdb/install"; \
		exit 2; \
	fi
	@if [ -n "$(CMAKE_PREFIX_PATH)" ]; then \
		first_path="$${CMAKE_PREFIX_PATH%%:*}"; \
		if [ ! -d "$$first_path" ]; then \
			echo "CMAKE_PREFIX_PATH not found: $$first_path"; \
			exit 2; \
		fi; \
	fi
	$(CMAKE) -S $(ROOT_DIR) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(CMAKE_TOOLCHAIN) $(CMAKE_PREFIX_OPT)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	$(BUILD_DIR)/s3gw_test_range

clean:
	rm -rf $(BUILD_DIR) $(ROOT_DIR)/bin
