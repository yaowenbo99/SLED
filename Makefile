# Offline build once dependencies have been installed (see make bootstrap).
BUILD_DIR ?= build
THIRD_PARTY_DIR ?= $(CURDIR)/third_party
CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I$(THIRD_PARTY_DIR)/duckdb/include -Ithird_party/nlohmann/include -I$(THIRD_PARTY_DIR)/boost/include
SOURCES := main.cpp $(wildcard src/*.cpp)
READ_MEMORY_SOURCES := dataset_read_memory.cpp src/io.cpp src/memory.cpp src/utils.cpp
HEADERS := $(wildcard src/*.h) third_party/nlohmann/include/nlohmann/json.hpp
LDLIBS := -L$(THIRD_PARTY_DIR)/duckdb/lib -lduckdb -pthread
RPATH := -Wl,-rpath,$(abspath $(THIRD_PARTY_DIR))/duckdb/lib

.PHONY: all build bootstrap test
all: build
build: $(BUILD_DIR)/sled $(BUILD_DIR)/dataset_read_memory
bootstrap:
	THIRD_PARTY_DIR="$(THIRD_PARTY_DIR)" bash scripts/bootstrap_deps.sh
$(BUILD_DIR):
	mkdir -p "$@"
$(BUILD_DIR)/sled: $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread $(CPPFLAGS) $(SOURCES) -o "$@" $(LDLIBS) $(RPATH)
$(BUILD_DIR)/dataset_read_memory: $(READ_MEMORY_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread $(CPPFLAGS) $(READ_MEMORY_SOURCES) -o "$@"
test: build
	python3 scripts/validate.py --executable "$(BUILD_DIR)/sled" --output "$(BUILD_DIR)/validation.json"
