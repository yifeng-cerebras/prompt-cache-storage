CXX := g++
GCC_VERSION := $(shell $(CXX) -dumpversion 2>/dev/null)
GCC_MAJOR := $(shell echo $(GCC_VERSION) | cut -d. -f1)
ifeq ($(GCC_MAJOR),)
  CXXSTD := -std=c++20
else ifeq ($(shell [ $(GCC_MAJOR) -lt 9 ] && echo yes),yes)
  CXXSTD := -std=gnu++2a
else
  CXXSTD := -std=c++20
endif
CXXFLAGS := $(CXXSTD) -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS := -lcurl -pthread

BIN_DIR := bin
SRC_DIR := src
TEST_DIR := tests

BIN := $(BIN_DIR)/prompt_cache_poc
TEST_PREFIX := $(BIN_DIR)/test_prefix_map
TEST_E2E := $(BIN_DIR)/test_e2e
TEST_S3 := $(BIN_DIR)/test_s3_integration
STRESS := $(BIN_DIR)/stress_e2e

SRCS := $(SRC_DIR)/cache.cc $(SRC_DIR)/s3_storage.cc $(SRC_DIR)/main.cpp

.PHONY: all clean test stress build unit

all: $(BIN)

build: all

$(BIN): $(SRCS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(TEST_PREFIX): $(TEST_DIR)/test_prefix_map.cpp $(SRC_DIR)/cache.cc $(SRC_DIR)/s3_storage.cc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_E2E): $(TEST_DIR)/test_e2e.cpp $(SRC_DIR)/cache.cc $(SRC_DIR)/s3_storage.cc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_S3): $(TEST_DIR)/test_s3_integration.cpp $(SRC_DIR)/s3_storage.cc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(STRESS): tools/stress_e2e.cpp $(SRC_DIR)/cache.cc $(SRC_DIR)/s3_storage.cc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test: $(TEST_PREFIX) $(TEST_E2E) $(TEST_S3)
	$(TEST_PREFIX)
	$(TEST_E2E)
	$(TEST_S3)

stress: $(STRESS)
unit: test
clean:
	rm -rf $(BIN_DIR)
