SHELL := /usr/bin/env bash

BOARD ?= host

BOARD_DIR := boards/$(BOARD)
BOARD_MK := $(BOARD_DIR)/board.mk
ifeq ($(wildcard $(BOARD_MK)),)
$(error unknown BOARD '$(BOARD)': missing $(BOARD_MK))
endif
include $(BOARD_MK)

TARGET ?= $(BOARD_TARGET)
PROFILE ?= $(BOARD_PROFILE)
PROFILE_MK := profiles/$(PROFILE).mk

TARGET_DIR := targets/$(TARGET)
TARGET_MK := $(TARGET_DIR)/target.mk
ifeq ($(wildcard $(TARGET_MK)),)
$(error unknown TARGET '$(TARGET)': missing $(TARGET_MK))
endif
include $(TARGET_MK)

ifneq ($(wildcard $(PROFILE_MK)),)
include $(PROFILE_MK)
endif
PROFILE_MK_DEPS := $(wildcard $(PROFILE_MK))

BOARD_HEADERS := $(wildcard $(BOARD_DIR)/*.h)
TARGET_HEADERS := $(wildcard $(TARGET_DIR)/*.h)
PROFILE_HEADERS := $(wildcard profiles/*.h)
PROFILE_MKS := $(wildcard profiles/*.mk)
TARGET_MAIN_SOURCE ?= targets/common/repl_main.c
TARGET_CC ?= cc
BUILD_DIR ?= build/$(BOARD)
ARTIFACT_ELF ?= $(BUILD_DIR)/frothy.elf
ARTIFACT_HEX ?= $(BUILD_DIR)/frothy.hex
ARTIFACT_MAP ?= $(BUILD_DIR)/frothy.map
ARTIFACT_SIZE ?= $(BUILD_DIR)/frothy.size

SOURCE_BASE := base/core.frothy
GEN_DIR := $(BUILD_DIR)/gen
SOURCE_BASE_C := $(GEN_DIR)/fr_source_base.c
SOURCE_BASE_H := $(GEN_DIR)/fr_source_base.h

ifeq ($(origin CC),default)
FR_CC := $(TARGET_CC)
else
FR_CC := $(CC)
endif

FR_CFLAGS := \
	-std=c99 \
	-Wall \
	-Wextra \
	-Werror \
	-pedantic \
	-Isrc \
	-I$(GEN_DIR) \
	-Iprofiles \
	-I$(TARGET_DIR) \
	-I$(BOARD_DIR) \
	-DFR_PROFILE_HEADER=\"$(PROFILE).h\" \
	-DFR_PROFILE_NAME=\"$(PROFILE)\" \
	$(TARGET_CFLAGS) \
	$(BOARD_CFLAGS) \
	$(CFLAGS)

FR_LDFLAGS := $(TARGET_LDFLAGS) $(BOARD_LDFLAGS) $(LDFLAGS)

COMPILER_SOURCES ?= \
	src/parse.c \
	src/compile.c

REPL_SOURCES ?= \
	src/repl.c \
	src/source_render.c

KERNEL_SOURCES = \
	src/types.c \
	src/tagged.c \
	src/crc.c \
	src/slot.c \
	src/profile.c \
	src/runtime.c \
	src/instruction.c \
	src/code.c \
	src/native.c \
	src/lib_native.c \
	src/handle.c \
	src/pad.c \
	src/object.c \
	src/image.c \
	src/base_defs.c \
	src/base_image.c \
	src/event.c \
	src/vm.c \
	$(SOURCE_BASE_C) \
	$(COMPILER_SOURCES) \
	$(REPL_SOURCES)

PERSISTENCE_KERNEL_SOURCES = \
	src/persist_format.c \
	src/persist.c \
	src/persist_payload.c

PLATFORM_SOURCES = \
	$(TARGET_SOURCES) \
	$(BOARD_SOURCES)

COMMON_TEST_SOURCES = \
	test/test.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES)

PERSISTENCE_SOURCES ?= $(PERSISTENCE_KERNEL_SOURCES)

# T12L: `frothy build` may pass FROTHY_LIB_NATIVES_C pointing at a generated
# strong override of fr_lib_natives. When set, suppress the empty defaults
# in src/lib_native.c via FR_LIB_NATIVES_PROVIDED so the override is the only
# definition. When unset, src/lib_native.c's empty defaults supply the
# symbols and no library natives are registered.
FROTHY_LIB_NATIVES_C ?=
FROTHY_LIBS_CMAKE ?=
FROTHY_LIB_EXT_SOURCES :=
FROTHY_LIB_EXT_INCLUDE_DIRS :=
ifneq ($(wildcard $(FROTHY_LIBS_CMAKE)),)
FROTHY_LIB_EXT_SOURCES := $(shell awk -v name=FROTHY_LIB_EXT_SOURCES '$$0 == "set(" name ")" { next } $$0 == "set(" name { in_list = 1; next } in_list && $$0 == ")" { in_list = 0; next } in_list { print }' "$(FROTHY_LIBS_CMAKE)")
FROTHY_LIB_EXT_INCLUDE_DIRS := $(shell awk -v name=FROTHY_LIB_EXT_INCLUDE_DIRS '$$0 == "set(" name ")" { next } $$0 == "set(" name { in_list = 1; next } in_list && $$0 == ")" { in_list = 0; next } in_list { print }' "$(FROTHY_LIBS_CMAKE)")
KERNEL_SOURCES += $(FROTHY_LIB_EXT_SOURCES)
FR_CFLAGS += $(addprefix -I,$(FROTHY_LIB_EXT_INCLUDE_DIRS))
endif
ifneq ($(FROTHY_LIB_NATIVES_C),)
KERNEL_SOURCES += $(FROTHY_LIB_NATIVES_C)
FR_CFLAGS += -DFR_LIB_NATIVES_PROVIDED
endif

TEST_SOURCES = \
	$(COMMON_TEST_SOURCES) \
	$(PERSISTENCE_SOURCES)

UNITY_TEST_SOURCES = \
	test/test_persist_atomicity.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_I2C_TEST_SOURCES = \
	test/test_i2c_registers.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_LIB_NATIVES_TEST_SOURCES = \
	test/test_lib_natives.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_PERSIST_TIER_TEST_SOURCES = \
	test/test_persist_tier.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T12_SERVO_TEST_SOURCES = \
	test/test_t12_servo.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T21_MEM_TEST_SOURCES = \
	test/test_t21_mem.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T15_NET_TEST_SOURCES = \
	test/test_t15_net.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T15B_TCP_TEST_SOURCES = \
	test/test_t15b_tcp.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T14_POWER_TEST_SOURCES = \
	test/test_t14_power.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

UNITY_T16_BYTES_TEST_SOURCES = \
	test/test_t16_bytes.c \
	test/unity/unity.c \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_KERNEL_SOURCES)

FROTHY_SOURCES = \
	$(TARGET_MAIN_SOURCE) \
	$(KERNEL_SOURCES) \
	$(PLATFORM_SOURCES) \
	$(PERSISTENCE_SOURCES)

KERNEL_DEPS = \
	src/config.h \
	src/froth.h \
	src/types.h src/types.c \
	src/tagged.h src/tagged.c \
	src/crc.h src/crc.c \
	src/slot.h src/slot.c \
	src/profile.h src/profile.c \
	src/runtime.h src/runtime.c \
	src/instruction.h src/instruction.c \
	src/code.h src/code.c \
	src/native.h src/native.c \
	src/lib_native.h src/lib_native.c \
	src/handle.h src/handle.c \
	src/pad.h src/pad.c \
	src/object.h src/object.c \
	src/image.h src/image.c \
	src/base_defs.h src/base_defs.c \
	src/base_image.h src/base_image.c \
	src/event.h src/event.c \
	src/platform.h \
	src/persist_format.h src/persist_format.c \
	src/persist.h src/persist.c \
	src/persist_payload.h src/persist_payload.c \
	src/vm.h src/vm.c \
	src/parse.h src/parse.c \
	src/compile.h src/compile.c \
	src/repl.h src/repl.c \
	src/source_render.h src/source_render.c \
	$(SOURCE_BASE_C) $(SOURCE_BASE_H) \
	$(PROFILE_HEADERS) \
	$(PROFILE_MKS)

BUILD_DEPS = \
	Makefile \
	$(BOARD_MK) \
	$(TARGET_MK) \
	$(BOARD_HEADERS) \
	$(TARGET_HEADERS) \
	$(PROFILE_MK_DEPS) \
	$(TARGET_SOURCES) \
	$(TARGET_BUILD_DEPS) \
	$(BOARD_SOURCES) \
	$(TARGET_MAIN_SOURCE)

TEST_DEPS = \
	test/test.c \
	$(KERNEL_DEPS) \
	$(BUILD_DEPS)

FROTHY_DEPS = \
	$(KERNEL_DEPS) \
	$(BUILD_DEPS) \
	$(FROTHY_LIB_NATIVES_C)

TEST_BINARY ?= test/test
UNITY_TEST_BINARY ?= $(BUILD_DIR)/test-unity
UNITY_I2C_TEST_BINARY ?= $(BUILD_DIR)/test-unity-i2c
UNITY_LIB_NATIVES_TEST_BINARY ?= $(BUILD_DIR)/test-unity-lib-natives
UNITY_PERSIST_TIER_TEST_BINARY ?= $(BUILD_DIR)/test-unity-persist-tier
UNITY_T12_SERVO_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t12-servo
UNITY_T21_MEM_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t21-mem
UNITY_T15_NET_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t15-net
UNITY_T15B_TCP_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t15b-tcp
UNITY_T14_POWER_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t14-power
UNITY_T16_BYTES_TEST_BINARY ?= $(BUILD_DIR)/test-unity-t16-bytes
FROTHY_BINARY ?= frothy
# Helper basename must match compilerProgramName in cmd/frothy-session.
OVERLAY_COMPILER ?= $(BUILD_DIR)/frothy-compile-overlay
FROTHY_HOST_COMMAND_BINARY ?= build/host/frothy
FROTHY_SESSION_BINARY ?= build/host/frothy-session
LIB_E2E_PROJECT ?= test/fixtures/projects/mixed-demo
LIB_E2E_BUILD_DIR ?= build/lib-e2e
LIB_E2E_BINARY ?= $(LIB_E2E_BUILD_DIR)/frothy.elf
GO_CACHE ?= $(abspath build/host/go-cache)
INSTALL_TEST_ROOT ?= build/install-host-root
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/frothy
INSTALL ?= install
INSTALL_DIR ?= $(INSTALL) -d
INSTALL_PROGRAM ?= $(INSTALL) -m 0755
HOST_INSTALL_BINDIR = $(DESTDIR)$(BINDIR)
HOST_INSTALL_LIBEXECDIR = $(DESTDIR)$(LIBEXECDIR)
# Vendor-SDK targets can delegate artifact work by setting TARGET_BUILD_COMMAND,
# TARGET_SIZE_COMMAND, TARGET_FLASH_DEPS, and TARGET_FLASH_COMMAND in target.mk.
ARTIFACTS = $(ARTIFACT_ELF) $(if $(TARGET_OBJCOPY),$(ARTIFACT_HEX)) $(if $(TARGET_SIZE)$(TARGET_SIZE_COMMAND),$(ARTIFACT_SIZE))
TARGET_FLASH_DEPS ?= $(ARTIFACT_HEX)
TARGET_ARTIFACTS_CHECK ?= @:
TARGET_FLASH_CHECK ?= @:

test: $(TEST_BINARY) ## Run the core C test binary.
	./$(TEST_BINARY)

test-unity: $(UNITY_TEST_BINARY) $(UNITY_I2C_TEST_BINARY) $(UNITY_LIB_NATIVES_TEST_BINARY) $(UNITY_PERSIST_TIER_TEST_BINARY) $(UNITY_T12_SERVO_TEST_BINARY) $(UNITY_T21_MEM_TEST_BINARY) $(UNITY_T15_NET_TEST_BINARY) $(UNITY_T15B_TCP_TEST_BINARY) $(UNITY_T14_POWER_TEST_BINARY) $(UNITY_T16_BYTES_TEST_BINARY) ## Run all Unity host binaries.
	./$(UNITY_TEST_BINARY)
	./$(UNITY_I2C_TEST_BINARY)
	./$(UNITY_LIB_NATIVES_TEST_BINARY)
	./$(UNITY_PERSIST_TIER_TEST_BINARY)
	./$(UNITY_T12_SERVO_TEST_BINARY)
	./$(UNITY_T21_MEM_TEST_BINARY)
	./$(UNITY_T15_NET_TEST_BINARY)
	./$(UNITY_T15B_TCP_TEST_BINARY)
	./$(UNITY_T14_POWER_TEST_BINARY)
	./$(UNITY_T16_BYTES_TEST_BINARY)
	$(MAKE) BOARD=host PROFILE=host_normal \
		UNITY_TEST_BINARY=build/host/test-unity-host-normal \
		UNITY_I2C_TEST_BINARY=build/host/test-unity-i2c-host-normal \
		UNITY_LIB_NATIVES_TEST_BINARY=build/host/test-unity-lib-natives-host-normal \
		UNITY_PERSIST_TIER_TEST_BINARY=build/host/test-unity-persist-tier-host-normal \
		UNITY_T12_SERVO_TEST_BINARY=build/host/test-unity-t12-servo-host-normal \
		UNITY_T21_MEM_TEST_BINARY=build/host/test-unity-t21-mem-host-normal \
		UNITY_T15_NET_TEST_BINARY=build/host/test-unity-t15-net-host-normal \
		UNITY_T15B_TCP_TEST_BINARY=build/host/test-unity-t15b-tcp-host-normal \
		UNITY_T14_POWER_TEST_BINARY=build/host/test-unity-t14-power-host-normal \
		UNITY_T16_BYTES_TEST_BINARY=build/host/test-unity-t16-bytes-host-normal \
		_test-unity-run

help: ## Show common make targets.
	@awk 'BEGIN { FS = ":.*##"; printf "Targets:\n" } /^[A-Za-z0-9_.-]+:.*##/ { printf "  %-38s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

_test-unity-run: $(UNITY_TEST_BINARY) $(UNITY_I2C_TEST_BINARY) $(UNITY_LIB_NATIVES_TEST_BINARY) $(UNITY_PERSIST_TIER_TEST_BINARY) $(UNITY_T12_SERVO_TEST_BINARY) $(UNITY_T21_MEM_TEST_BINARY) $(UNITY_T15_NET_TEST_BINARY) $(UNITY_T15B_TCP_TEST_BINARY) $(UNITY_T14_POWER_TEST_BINARY) $(UNITY_T16_BYTES_TEST_BINARY)
	./$(UNITY_TEST_BINARY)
	./$(UNITY_I2C_TEST_BINARY)
	./$(UNITY_LIB_NATIVES_TEST_BINARY)
	./$(UNITY_PERSIST_TIER_TEST_BINARY)
	./$(UNITY_T12_SERVO_TEST_BINARY)
	./$(UNITY_T21_MEM_TEST_BINARY)
	./$(UNITY_T15_NET_TEST_BINARY)
	./$(UNITY_T15B_TCP_TEST_BINARY)
	./$(UNITY_T14_POWER_TEST_BINARY)
	./$(UNITY_T16_BYTES_TEST_BINARY)

ifneq ($(FROTHY_BINARY),frothy)
frothy: $(FROTHY_BINARY)
endif

$(FROTHY_BINARY): $(FROTHY_DEPS) | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) $(FROTHY_SOURCES) $(FR_LDFLAGS) -o $@

artifacts: $(ARTIFACTS) ## Build firmware artifacts for the selected board.
	$(TARGET_ARTIFACTS_CHECK)

$(BUILD_DIR):
	mkdir -p $@

# Bake base/core.frothy into a C object the boot compiler reads. Hex encoding
# only -- the same compiler that handles REPL input compiles these bytes. The
# generated files live under build/ and are never checked in.
$(SOURCE_BASE_H): Makefile | $(BUILD_DIR)
	@mkdir -p $(GEN_DIR)
	printf '#pragma once\n#include "config.h"\n#include <stdint.h>\n#if FR_FEATURE_SOURCE_BASE\nextern const char fr_source_base_bytes[];\nextern const uint16_t fr_source_base_bytes_len;\n#endif\n' > $@

$(SOURCE_BASE_C): $(SOURCE_BASE) Makefile | $(BUILD_DIR)
	@mkdir -p $(GEN_DIR)
	{ printf '#include "fr_source_base.h"\n#if FR_FEATURE_SOURCE_BASE\nconst char fr_source_base_bytes[] = {\n'; \
	  xxd -i < $(SOURCE_BASE); \
	  printf '};\nconst uint16_t fr_source_base_bytes_len = %s;\n#endif\n' "$$(wc -c < $(SOURCE_BASE) | tr -d ' ')"; \
	} > $@

ifneq ($(TARGET_BUILD_COMMAND),)
$(ARTIFACT_ELF): $(FROTHY_DEPS) | $(BUILD_DIR)
	$(TARGET_BUILD_COMMAND)
else
$(ARTIFACT_ELF): $(FROTHY_DEPS) | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) $(FROTHY_SOURCES) $(FR_LDFLAGS) $(TARGET_MAP_LDFLAG) -o $@
endif

$(ARTIFACT_HEX): $(ARTIFACT_ELF)
	$(TARGET_OBJCOPY) -O ihex -R .eeprom $< $@

ifneq ($(TARGET_SIZE_COMMAND),)
$(ARTIFACT_SIZE): $(ARTIFACT_ELF)
	$(TARGET_SIZE_COMMAND)
else
$(ARTIFACT_SIZE): $(ARTIFACT_ELF)
	$(TARGET_SIZE) -A $< > $@
	$(TARGET_SIZE) $< >> $@
endif

flash: $(TARGET_FLASH_DEPS) ## Flash the selected board; requires BOARD_PORT.
	@if [ -z "$(TARGET_FLASH_COMMAND)" ]; then \
		printf 'target %s does not define a flash command\n' "$(TARGET)"; \
		exit 2; \
	fi
	@if [ -z "$(BOARD_PORT)" ]; then \
		printf 'BOARD_PORT is required, for example BOARD_PORT=/dev/cu.usbmodemXXXX\n'; \
		exit 2; \
	fi
	$(TARGET_FLASH_CHECK)
	$(TARGET_FLASH_COMMAND)

# Offsets are the default ESP-IDF singleapp NVS range (partitions_singleapp.csv,
# PARTITION_TABLE_OFFSET=0x8000, 0x1000 table). Update in lockstep with any
# custom partition table.
wipe-nvs: ## Erase the ESP32 NVS region used by Frothy persistence.
	@if [ "$(BOARD)" != "esp32_devkit_v1" ]; then \
		printf 'wipe-nvs: unsupported BOARD "%s"; only esp32_devkit_v1 is supported\n' "$(BOARD)"; \
		exit 2; \
	fi
	@if [ -z "$(BOARD_PORT)" ]; then \
		printf 'BOARD_PORT is required, for example BOARD_PORT=/dev/cu.usbserial-0001\n'; \
		exit 2; \
	fi
	. "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && esptool.py --chip esp32 --port "$(BOARD_PORT)" erase_region 0x9000 0x6000

# Merged .bin for the web flasher (T13b D11). Local esptool.py is
# v4.12.dev1 which only accepts merge_bin (underscore).
web-bins: ## Build the merged ESP32 binary for the web flasher.
	$(MAKE) BOARD=esp32_devkit_v1 PROFILE=esp32_plain artifacts
	@mkdir -p web/flash/firmware
	. "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && esptool.py --chip esp32 merge_bin \
		-o web/flash/firmware/esp32_devkit_v1-esp32_plain.bin \
		0x1000 build/esp32_devkit_v1/bootloader/bootloader.bin \
		0x8000 build/esp32_devkit_v1/partition_table/partition-table.bin \
		0x10000 build/esp32_devkit_v1/frothy.bin

test-host-normal: ## Run the core C suite with the host_normal profile.
	$(MAKE) BOARD=host PROFILE=host_normal \
		TEST_BINARY=test/test-host-normal test

host-normal:
	$(MAKE) BOARD=host PROFILE=host_normal \
		FROTHY_BINARY=build/host/frothy-host-normal frothy

examples: host-normal ## Run host-safe examples through the host_normal REPL.
	tools/run-examples.sh

examples-manifest: ## Regenerate the editor example manifests from examples/.
	node tools/gen-examples-manifest.mjs

check-examples-manifest: examples-manifest ## Fail if the manifests are stale.
	git diff --exit-code -- libs/frothy-editor/src/examples.generated.ts editors/vscode/src/examples.generated.ts

test-host-normal-transcript: host-normal ## Replay the host_normal transcript.
	@out=$$(printf '%s\n' \
		'status' \
		'time is 200' \
		'myblink is fn [ pin: $$led_builtin, 1; ms: time; pin: $$led_builtin, 0; ms: time ]' \
		'blink_times is fn with count [ repeat count [ myblink: ] ]' \
		'boot is fn [ blink_times: 3 ]' \
		'gpio.high: $$led_builtin' \
		'1000 + gpio.read: $$led_builtin' \
		'see gpio.high' \
		'gpio.low: $$led_builtin' \
		'gpio.read: $$led_builtin' \
		'gpio.high: $$led_builtin' \
		'gpio.toggle: $$led_builtin' \
		'gpio.read: $$led_builtin' \
		'led.on:' \
		'gpio.read: $$led_builtin' \
		'led.off:' \
		'gpio.read: $$led_builtin' \
		'wrap: 7, 3' \
		'wrap: -1, 3' \
		'sign: -5' \
		'sign: 0' \
		'sign: 7' \
		'random.chance?: 0, 100' \
		'random.chance?: 100, 100' \
		'random.chance?: 0, 0' \
		'random.percent?: 100' \
		'random.percent?: 0' \
		'counter is cells(1)' \
		'set counter[0] to 7' \
		'counter[0]' \
		'message is "ready"' \
		'message' \
		'pad.reset' \
		'pad.emit-byte: 65' \
		'pad.emit-byte: 10' \
		'pad.len:' \
		'pad.type' \
		'say_ready is fn [ print: "inner"; print: "\\n" ]' \
		'say_ready:' \
		'say_bytes is fn [ print: bytes.from-text: "bytes\\n" ]' \
		'say_bytes:' \
		'status is cells(1)' \
		'set status[0] to message' \
		'status[0]' \
		'led_ref is led.on' \
		'save' \
		'clear' \
		'restore' \
		'counter[0]' \
		'status[0]' \
		'see message' \
		'see counter' \
		'see led_ref' \
		'boot:' \
		'see boot' \
		'words' \
		| build/host/frothy-host-normal); \
	ok_count=$$(printf '%s\n' "$$out" | grep -c 'ok$$'); \
	if [ "$$ok_count" != 56 ]; then \
		printf '%s\n' "$$out"; \
		exit 1; \
	fi; \
	for expected in \
		'profile=host_normal' \
		'compiler=device' \
		'names=device' \
		'storage=eeprom' \
		'overlay cells 1' \
		'7' \
		'"ready"' \
		'overlay text 5' \
		'pad.emit-byte' \
		'> A' \
		'> inner' \
		'> bytes' \
		'2' \
		'overlay code' \
		'1001' \
		'to boot [ blink_times: 3 ]' \
		'to gpio.high with pin [ gpio.write: pin, 1 ]' \
		'true' \
		'false' \
		'wrap random.chance? random.percent? sign adc.percent time myblink blink_times'; do \
		if ! printf '%s\n' "$$out" | grep -qF "$$expected"; then \
			printf '%s\nmissing expected text: %s\n' "$$out" "$$expected"; \
			exit 1; \
		fi; \
	done; \
	err_out=$$(printf '%s\n' \
		'bad is fn [ pin: ]' \
		'time is 200' \
		'words' \
		| build/host/frothy-host-normal); \
	if ! printf '%s\n' "$$err_out" | grep -q 'error: bad source (8)'; then \
		printf '%s\nmissing bad-source error\n' "$$err_out"; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$err_out" | grep -q 'time'; then \
		printf '%s\nmissing recovery command output\n' "$$err_out"; \
		exit 1; \
	fi; \
	printf 'host_normal transcript ok\n'

host-normal-events:
	$(MAKE) BOARD=host PROFILE=host_normal \
		FROTHY_BINARY=build/host/frothy-host-normal-events \
		CFLAGS=-DFR_INCLUDE_TEST_NATIVES=1 frothy

# Exercises the T11a save -> reinstall-base -> restore -> fire-event
# round-trip through the CLI. Needs FR_INCLUDE_TEST_NATIVES so
# frothy.fire-event is reachable from the REPL.
test-host-normal-event-transcript: host-normal-events ## Replay the host_normal event transcript.
	@out=$$(printf '%s\n' \
		'counter is cells(1)' \
		'set counter[0] to 1' \
		'mark is fn [ set counter[0] to 42 ]' \
		'boot is fn [ on 7 rising [ mark: ] ]' \
		'boot:' \
		'counter[0]' \
		'save' \
		'clear' \
		'restore' \
		'counter[0]' \
		'frothy.fire-event: "on", 7, "rising"' \
		'counter[0]' \
		| build/host/frothy-host-normal-events); \
	ok_count=$$(printf '%s\n' "$$out" | grep -c '^> ok$$'); \
	if [ "$$ok_count" != 9 ]; then \
		printf '%s\n' "$$out"; \
		printf 'expected 9 command-only ok lines, got %s\n' "$$ok_count"; \
		exit 1; \
	fi; \
	one_count=$$(printf '%s\n' "$$out" | grep -c '^> 1$$'); \
	if [ "$$one_count" != 2 ]; then \
		printf '%s\n' "$$out"; \
		printf 'expected counter[0] to read 1 before and after restore (got %s)\n' "$$one_count"; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$out" | grep -q '^> 42$$'; then \
		printf '%s\n' "$$out"; \
		printf 'expected counter[0] to read 42 after fire-event\n'; \
		exit 1; \
	fi; \
	printf 'host_normal event transcript ok\n'

test-host-normal-profile: test-host-normal test-host-normal-transcript test-host-normal-event-transcript

test-lib-e2e: frothy-host-command ## Build and run the library extension e2e fixture.
	BUILD_DIR="$(abspath $(LIB_E2E_BUILD_DIR))" "$(abspath $(FROTHY_HOST_COMMAND_BINARY))" build --project "$(abspath $(LIB_E2E_PROJECT))"
	@out=$$(printf '%s\n' \
		'test-mixed.echo:' \
		'words' \
		| "$(LIB_E2E_BINARY)"); \
	if ! printf '%s\n' "$$out" | grep -q '^> 7$$'; then \
		printf '%s\nmissing library native result: 7\n' "$$out"; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$out" | grep -qF 'test-mixed.echo'; then \
		printf '%s\nmissing library native in words output: test-mixed.echo\n' "$$out"; \
		exit 1; \
	fi; \
	printf 'lib e2e ok\n'

esp32-plain-host:
	$(MAKE) BOARD=esp32_devkit_v1 TARGET=host PROFILE=esp32_plain \
		BUILD_DIR=build/esp32-plain-host \
		FROTHY_BINARY=build/esp32-plain-host/frothy frothy

test-esp32-plain-host-transcript: esp32-plain-host ## Replay the esp32_plain profile on the host target.
	@out=$$(printf '%s\n' \
		'status' \
		'words' \
		'$$led_builtin' \
		'$$a0' \
		'pin: $$led_builtin, 1' \
		'gpio.read: $$led_builtin' \
		'adc.read: $$a0' \
		'message is "ready"' \
		'message' \
		'status is cells(1)' \
		'set status[0] to message' \
		'boot is fn [ pin: $$led_builtin, 1 ]' \
		'gpio.write: $$led_builtin, 0' \
		'gpio.high: $$led_builtin' \
		'1000 + gpio.read: $$led_builtin' \
		'see gpio.high' \
		'gpio.low: $$led_builtin' \
		'gpio.read: $$led_builtin' \
		'led.on:' \
		'gpio.read: $$led_builtin' \
		'sign: -5' \
		'random.chance?: 100, 100' \
		'save' \
		'clear' \
		'restore' \
		'status[0]' \
		'see message' \
		'see status' \
		'boot:' \
		'see boot' \
		| build/esp32-plain-host/frothy); \
	ok_count=$$(printf '%s\n' "$$out" | grep -c 'ok$$'); \
	if [ "$$ok_count" != 30 ]; then \
		printf '%s\n' "$$out"; \
		exit 1; \
	fi; \
	for expected in \
		'profile=esp32_plain' \
		'compiler=device' \
		'names=device' \
		'storage=eeprom' \
		'$$a0' \
		'$$boot_button' \
		'2' \
		'34' \
		'1001' \
		'512' \
		'"ready"' \
		'overlay text 5' \
		'overlay cells 1' \
		'to boot [ gpio.write: $$led_builtin, 1 ]' \
		'to gpio.high with pin [ gpio.write: pin, 1 ]' \
		'true'; do \
		if ! printf '%s\n' "$$out" | grep -qF "$$expected"; then \
			printf '%s\nmissing expected text: %s\n' "$$out" "$$expected"; \
			exit 1; \
		fi; \
	done; \
	printf 'esp32_plain host transcript ok\n'

host-overlay-compiler:
	$(MAKE) BOARD=host PROFILE=host_small \
		OVERLAY_COMPILER=build/host/frothy-compile-overlay \
		build/host/frothy-compile-overlay

frothy-host-command: host-overlay-compiler ## Build the user-facing frothy CLI binary.
	GOCACHE=$(GO_CACHE) go build -o $(FROTHY_HOST_COMMAND_BINARY) ./cmd/frothy-session

# Build the user-facing `frothy` CLI for local use without a system install.
# It lands next to its frothy-compile-overlay helper (resolved as a sibling),
# so the only step left is putting that directory on PATH — which the recipe
# prints. No symlinking required.
cli: frothy-host-command ## Build the local frothy CLI and print PATH setup.
	@bindir='$(abspath $(dir $(FROTHY_HOST_COMMAND_BINARY)))'; \
	printf '\n  \033[32m✓\033[0m Built the frothy CLI  ->  %s/frothy\n\n' "$$bindir"; \
	printf '  Add it to your PATH for this shell:\n\n'; \
	printf '      \033[1mexport PATH="%s:$$PATH"\033[0m\n\n' "$$bindir"; \
	printf '  To keep it, append that line to your shell profile\n'; \
	printf '  (~/.zshrc or ~/.bashrc), then restart your terminal or run:\n\n'; \
	printf '      \033[1msource ~/.zshrc\033[0m\n\n'; \
	printf '  Then run \033[1mfrothy --help\033[0m from anywhere.\n\n'

frothy-session: host-overlay-compiler
	GOCACHE=$(GO_CACHE) go build -o $(FROTHY_SESSION_BINARY) ./cmd/frothy-session

install-host: frothy-host-command frothy-session host-overlay-compiler ## Install host binaries under DESTDIR/PREFIX.
	$(INSTALL_DIR) "$(HOST_INSTALL_BINDIR)"
	$(INSTALL_DIR) "$(HOST_INSTALL_LIBEXECDIR)"
	$(INSTALL_PROGRAM) "$(FROTHY_HOST_COMMAND_BINARY)" "$(HOST_INSTALL_BINDIR)/frothy"
	$(INSTALL_PROGRAM) "$(FROTHY_SESSION_BINARY)" "$(HOST_INSTALL_BINDIR)/frothy-session"
	$(INSTALL_PROGRAM) "$(OVERLAY_COMPILER)" "$(HOST_INSTALL_LIBEXECDIR)/frothy-compile-overlay"

test-install-host:
	rm -rf "$(INSTALL_TEST_ROOT)"
	$(MAKE) install-host DESTDIR="$(abspath $(INSTALL_TEST_ROOT))" PREFIX=/usr/local
	@out=$$(cd "$(abspath $(INSTALL_TEST_ROOT))" && printf '%s\n' \
		'time is 200' \
		'blink is fn [ ms: time ]' \
		'blink:' \
		| env -i PATH=/usr/bin:/bin "$(abspath $(INSTALL_TEST_ROOT))/usr/local/bin/frothy" session --dry-run); \
	apply_count=$$(printf '%s\n' "$$out" | grep -c '^apply '); \
	run_count=$$(printf '%s\n' "$$out" | grep -c '^run '); \
	if [ "$$apply_count" != 2 ] || [ "$$run_count" != 1 ]; then \
		printf '%s\n' "$$out"; \
		exit 1; \
	fi; \
	printf 'install host dry-run ok\n'

$(TEST_BINARY): $(TEST_DEPS)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_TEST_BINARY): $(UNITY_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_I2C_TEST_BINARY): $(UNITY_I2C_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_I2C_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_LIB_NATIVES_TEST_BINARY): $(UNITY_LIB_NATIVES_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_LIB_NATIVES_PROVIDED=1 $(UNITY_LIB_NATIVES_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_PERSIST_TIER_TEST_BINARY): $(UNITY_PERSIST_TIER_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_LIB_NATIVES_PROVIDED=1 $(UNITY_PERSIST_TIER_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T12_SERVO_TEST_BINARY): $(UNITY_T12_SERVO_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_T12_SERVO_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T21_MEM_TEST_BINARY): $(UNITY_T21_MEM_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 $(UNITY_T21_MEM_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T15_NET_TEST_BINARY): $(UNITY_T15_NET_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_T15_NET_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T15B_TCP_TEST_BINARY): $(UNITY_T15B_TCP_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_T15B_TCP_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T14_POWER_TEST_BINARY): $(UNITY_T14_POWER_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_T14_POWER_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_T16_BYTES_TEST_BINARY): $(UNITY_T16_BYTES_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) -DFR_INCLUDE_TEST_NATIVES=1 -DFR_HOST_TEST_HELPERS=1 $(UNITY_T16_BYTES_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(OVERLAY_COMPILER): tools/frothy-compile-overlay.c $(FROTHY_DEPS) | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) tools/frothy-compile-overlay.c $(KERNEL_SOURCES) $(PLATFORM_SOURCES) $(PERSISTENCE_SOURCES) $(FR_LDFLAGS) -o $@

print-config: ## Print the selected board, target, profile, and build paths.
	@printf 'BOARD=%s\n' "$(BOARD)"
	@printf 'BOARD_DIR=%s\n' "$(BOARD_DIR)"
	@printf 'BOARD_MK=%s\n' "$(BOARD_MK)"
	@printf 'BOARD_SOURCES=%s\n' "$(BOARD_SOURCES)"
	@printf 'TARGET=%s\n' "$(TARGET)"
	@printf 'TARGET_DIR=%s\n' "$(TARGET_DIR)"
	@printf 'TARGET_MK=%s\n' "$(TARGET_MK)"
	@printf 'TARGET_SOURCES=%s\n' "$(TARGET_SOURCES)"
	@printf 'PROFILE=%s\n' "$(PROFILE)"
	@printf 'PROFILE_MK=%s\n' "$(PROFILE_MK)"
	@printf 'PROFILE_HEADER=%s\n' "$(PROFILE).h"
	@printf 'CC=%s\n' "$(FR_CC)"
	@printf 'COMPILER_SOURCES=%s\n' "$(COMPILER_SOURCES)"
	@printf 'REPL_SOURCES=%s\n' "$(REPL_SOURCES)"
	@printf 'PERSISTENCE_SOURCES=%s\n' "$(PERSISTENCE_SOURCES)"
	@printf 'BUILD_DIR=%s\n' "$(BUILD_DIR)"
	@printf 'ARTIFACT_ELF=%s\n' "$(ARTIFACT_ELF)"
	@printf 'ARTIFACT_HEX=%s\n' "$(ARTIFACT_HEX)"
	@printf 'ARTIFACT_SIZE=%s\n' "$(ARTIFACT_SIZE)"

vsix: ## Build the VS Code extension package.
	cd editors/vscode && npm ci && npm run build && npx vsce package

clean: ## Remove generated build outputs.
	rm -rf build frothy test/test test/test-host-normal test/fixtures/projects/*/.frothy

.PHONY: test test-unity help artifacts flash wipe-nvs web-bins test-host-normal host-normal examples examples-manifest check-examples-manifest host-normal-events test-host-normal-transcript test-host-normal-event-transcript test-host-normal-profile test-lib-e2e esp32-plain-host test-esp32-plain-host-transcript host-overlay-compiler frothy-host-command frothy-session cli install-host test-install-host print-config vsix clean
