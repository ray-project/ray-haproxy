# Extend HAProxy's Makefile: make -f Makefile -f /path/to/stream-metrics.mk.
# COPTS carries the exact compiler, debug and feature definitions used by HAProxy.
STREAM_METRICS_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
STREAM_METRICS_BUILD_DIR ?= $(CURDIR)/stream-build
STREAM_METRICS_MODULE_DIR ?= $(CURDIR)/lib
HAPROXY_BINARY ?= $(CURDIR)/haproxy

ifndef CJSON_SOURCE_DIR
$(error CJSON_SOURCE_DIR must point to the pinned upstream Lua CJSON sources)
endif

STREAM_METRICS_SOURCE := $(STREAM_METRICS_ROOT)/src/stream_metrics.c
STREAM_METRICS_JSON := $(addprefix $(CJSON_SOURCE_DIR)/,lua_cjson.c strbuf.c fpconv.c)
STREAM_METRICS_DEPS := $(STREAM_METRICS_SOURCE) $(STREAM_METRICS_JSON) \
  $(wildcard $(CJSON_SOURCE_DIR)/*.h) $(DEP) \
  $(STREAM_METRICS_ROOT)/ci/build/stream-metrics.mk
STREAM_METRICS_FLAGS := $(COPTS) -I$(CJSON_SOURCE_DIR) -std=gnu11 -pthread \
  -Wno-declaration-after-statement -Wno-undef \
  -Dluaopen_cjson=luaopen_ray_stream_metrics_impl \
  -Dluaopen_cjson_safe=luaopen_ray_stream_metrics_safe \
  -DCJSON_MODNAME='"ray_stream_metrics"'
STREAM_METRICS_MODULE := $(STREAM_METRICS_MODULE_DIR)/ray_stream_metrics.so
STREAM_METRICS_TEST := $(STREAM_METRICS_BUILD_DIR)/stream_metrics_tests

.PHONY: ray-stream-metrics check-stream-metrics stream-metrics-manifest
all: ray-stream-metrics
ray-stream-metrics: $(STREAM_METRICS_MODULE)

$(STREAM_METRICS_MODULE): $(STREAM_METRICS_DEPS) $(HAPROXY_BINARY)
	@mkdir -p "$(STREAM_METRICS_MODULE_DIR)"
	@set -e; \
	  abi=$$("$(HAPROXY_BINARY)" -v | awk 'NR == 1 {print $$3}'); \
	  test -n "$$abi"; \
	  $(CC) $(STREAM_METRICS_FLAGS) -O3 -fPIC -shared \
	    -DRAY_HAPROXY_ABI="\"$$abi\"" -o "$@" \
	    $(STREAM_METRICS_SOURCE) $(STREAM_METRICS_JSON) -pthread -lm

# Compile the actual parser/ring with only HAProxy adapter callbacks excluded.
# Assertions remain enabled even if HAProxy's caller supplies -DNDEBUG.
$(STREAM_METRICS_TEST): $(STREAM_METRICS_DEPS) \
  $(STREAM_METRICS_ROOT)/tests/stream_metrics_test.c $(LUA_LIB)/liblua.a \
  $(UNITY_SOURCE_DIR)/unity.c $(UNITY_SOURCE_DIR)/unity.h $(UNITY_SOURCE_DIR)/unity_internals.h
	@mkdir -p "$(STREAM_METRICS_BUILD_DIR)"
	$(CC) $(STREAM_METRICS_FLAGS) -I$(UNITY_SOURCE_DIR) -O1 -g -UNDEBUG \
	  -DRAY_STREAM_METRICS_UNIT_TEST -ffunction-sections -fdata-sections \
	  -fno-omit-frame-pointer -fsanitize=address,undefined \
	  -o "$@" $(STREAM_METRICS_ROOT)/tests/stream_metrics_test.c \
	  $(STREAM_METRICS_JSON) $(UNITY_SOURCE_DIR)/unity.c $(LUA_LIB)/liblua.a \
	  -Wl,--gc-sections -pthread -lm -ldl -fsanitize=address,undefined

# Mandatory in the wheel CI build on both architectures. Stop on any failure.
check-stream-metrics: $(STREAM_METRICS_TEST)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	  timeout 30 "$(STREAM_METRICS_TEST)"

# Run after stripping/RPATH adjustment so hashes describe the shipped artifact.
stream-metrics-manifest: $(STREAM_METRICS_MODULE)
	@set -e; \
	  abi=$$("$(HAPROXY_BINARY)" -v | awk 'NR == 1 {print $$3}'); \
	  source_hash=$$(sha256sum "$(STREAM_METRICS_SOURCE)" | awk '{print $$1}'); \
	  module_hash=$$(sha256sum "$(STREAM_METRICS_MODULE)" | awk '{print $$1}'); \
	  printf '{\n  "haproxy_abi": "%s",\n  "source_sha256": "%s",\n  "module_sha256": "%s"\n}\n' \
	    "$$abi" "$$source_hash" "$$module_hash" > "$(STREAM_METRICS_MODULE_DIR)/ray_stream_metrics.json"
