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
	src/repl.c

KERNEL_SOURCES = \
	src/tagged.c \
	src/crc.c \
	src/slot.c \
	src/profile.c \
	src/runtime.c \
	src/instruction.c \
	src/code.c \
	src/native.c \
	src/handle.c \
	src/pad.c \
	src/object.c \
	src/image.c \
	src/base_defs.c \
	src/base_image.c \
	src/vm.c \
	$(COMPILER_SOURCES) \
	$(REPL_SOURCES)

PERSISTENCE_KERNEL_SOURCES = \
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

TEST_SOURCES = \
	$(COMMON_TEST_SOURCES) \
	$(PERSISTENCE_SOURCES)

UNITY_TEST_SOURCES = \
	test/test_persist_atomicity.c \
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
	src/types.h \
	src/tagged.h src/tagged.c \
	src/crc.h src/crc.c \
	src/slot.h src/slot.c \
	src/profile.h src/profile.c \
	src/runtime.h src/runtime.c \
	src/instruction.h src/instruction.c \
	src/code.h src/code.c \
	src/native.h src/native.c \
	src/handle.h src/handle.c \
	src/pad.h src/pad.c \
	src/object.h src/object.c \
	src/image.h src/image.c \
	src/base_defs.h src/base_defs.c \
	src/base_image.h src/base_image.c \
	src/platform.h \
	src/persist.h src/persist.c \
	src/persist_payload.h src/persist_payload.c \
	src/vm.h src/vm.c \
	src/parse.h src/parse.c \
	src/compile.h src/compile.c \
	src/repl.h src/repl.c \
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
	$(BUILD_DEPS)

TEST_BINARY ?= test/test
UNITY_TEST_BINARY ?= $(BUILD_DIR)/test-unity
FROTHY_BINARY ?= frothy
# Helper basename must match compilerProgramName in cmd/frothy-session.
OVERLAY_COMPILER ?= $(BUILD_DIR)/frothy-compile-overlay
FROTHY_HOST_COMMAND_BINARY ?= build/host/frothy
FROTHY_SESSION_BINARY ?= build/host/frothy-session
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

test: $(TEST_BINARY)
	./$(TEST_BINARY)

test-unity: $(UNITY_TEST_BINARY)
	./$(UNITY_TEST_BINARY)

ifneq ($(FROTHY_BINARY),frothy)
frothy: $(FROTHY_BINARY)
endif

$(FROTHY_BINARY): $(FROTHY_DEPS) | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) $(FROTHY_SOURCES) $(FR_LDFLAGS) -o $@

artifacts: $(ARTIFACTS)

$(BUILD_DIR):
	mkdir -p $@

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

flash: $(TARGET_FLASH_DEPS)
	@if [ -z "$(TARGET_FLASH_COMMAND)" ]; then \
		printf 'target %s does not define a flash command\n' "$(TARGET)"; \
		exit 2; \
	fi
	@if [ -z "$(BOARD_PORT)" ]; then \
		printf 'BOARD_PORT is required, for example BOARD_PORT=/dev/cu.usbmodemXXXX\n'; \
		exit 2; \
	fi
	$(TARGET_FLASH_COMMAND)

test-tiny-328:
	$(MAKE) BOARD=host PROFILE=tiny_328 TEST_BINARY=test/test-tiny-328 test

test-tiny-328-volatile:
	$(MAKE) BOARD=host PROFILE=tiny_328_volatile \
		PERSISTENCE_SOURCES= TEST_BINARY=test/test-tiny-328-volatile test

test-tiny-328-tethered:
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered \
		TEST_BINARY=test/test-tiny-328-tethered test

test-tiny-328-tethered-host-names:
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered_host_names \
		TEST_BINARY=test/test-tiny-328-tethered-host-names test

test-tiny-328-tethered-host-names-persist:
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered_host_names_persist \
		TEST_BINARY=test/test-tiny-328-tethered-host-names-persist test

test-host-normal:
	$(MAKE) BOARD=host PROFILE=host_normal \
		TEST_BINARY=test/test-host-normal test

host-normal:
	$(MAKE) BOARD=host PROFILE=host_normal \
		FROTHY_BINARY=build/host/frothy-host-normal frothy

test-host-normal-transcript: host-normal
	@out=$$(printf '%s\n' \
		'status' \
		'time is 200' \
		'blink is fn [ pin: $$led_builtin, 1; ms: time; pin: $$led_builtin, 0; ms: time ]' \
		'blink_times is fn with count [ repeat count [ blink: ] ]' \
		'boot is fn [ blink_times: 3 ]' \
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
		'status is cells(1)' \
		'set status[0] to message' \
		'status[0]' \
		'save' \
		'clear' \
		'restore' \
		'counter[0]' \
		'status[0]' \
		'see message' \
		'see counter' \
		'boot:' \
		'see boot' \
		'words' \
		| build/host/frothy-host-normal); \
	ok_count=$$(printf '%s\n' "$$out" | grep -c 'ok$$'); \
	if [ "$$ok_count" != 28 ]; then \
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
		'2' \
		'overlay code' \
		'CALL_SLOT_ARG 42 1' \
		'time blink blink_times'; do \
		if ! printf '%s\n' "$$out" | grep -q "$$expected"; then \
			printf '%s\nmissing expected text: %s\n' "$$out" "$$expected"; \
			exit 1; \
		fi; \
	done; \
	err_out=$$(printf '%s\n' \
		'bad is fn [ pin: ]' \
		'time is 200' \
		'words' \
		| build/host/frothy-host-normal); \
	if ! printf '%s\n' "$$err_out" | grep -q 'err 8'; then \
		printf '%s\nmissing bad-source error\n' "$$err_out"; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$err_out" | grep -q 'time'; then \
		printf '%s\nmissing recovery command output\n' "$$err_out"; \
		exit 1; \
	fi; \
	printf 'host_normal transcript ok\n'

test-host-normal-profile: test-host-normal test-host-normal-transcript

esp32-plain-host:
	$(MAKE) BOARD=esp32_devkit_v1 TARGET=host PROFILE=esp32_plain \
		BUILD_DIR=build/esp32-plain-host \
		FROTHY_BINARY=build/esp32-plain-host/frothy frothy

test-esp32-plain-host-transcript: esp32-plain-host
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
	if [ "$$ok_count" != 20 ]; then \
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
		'1' \
		'512' \
		'"ready"' \
		'overlay text 5' \
		'overlay cells 1' \
		'CALL_NATIVE_SLOT 3'; do \
		if ! printf '%s\n' "$$out" | grep -q "$$expected"; then \
			printf '%s\nmissing expected text: %s\n' "$$out" "$$expected"; \
			exit 1; \
		fi; \
	done; \
	printf 'esp32_plain host transcript ok\n'

host-overlay-compiler:
	$(MAKE) BOARD=host PROFILE=tiny_328_tether_compiler \
		OVERLAY_COMPILER=build/host/frothy-compile-overlay \
		build/host/frothy-compile-overlay

host-overlay-compiler-tiny-host-names:
	$(MAKE) BOARD=host PROFILE=tiny_328_tether_compiler_host_names \
		OVERLAY_COMPILER=build/host/frothy-compile-overlay-tiny-host-names \
		build/host/frothy-compile-overlay-tiny-host-names

frothy-host-command: host-overlay-compiler
	GOCACHE=$(GO_CACHE) go build -o $(FROTHY_HOST_COMMAND_BINARY) ./cmd/frothy-session

frothy-session: host-overlay-compiler
	GOCACHE=$(GO_CACHE) go build -o $(FROTHY_SESSION_BINARY) ./cmd/frothy-session

install-host: frothy-host-command frothy-session host-overlay-compiler
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

test-helper-targets: host-overlay-compiler host-overlay-compiler-tiny-host-names
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered \
		FROTHY_BINARY=build/host/frothy-tethered frothy
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered_host_names_persist \
		FROTHY_BINARY=build/host/frothy-host-names-persist frothy
	@helper_target=$$(printf '@target\n' | build/host/frothy-compile-overlay); \
	device_status=$$(printf 'status\n' | build/host/frothy-tethered); \
	for field in profile_hash word_size int_min int_max apply_bytes; do \
		helper_value=$$(printf '%s\n' "$$helper_target" | sed -n "s/.*$$field=\\([^ ]*\\).*/\\1/p"); \
		device_value=$$(printf '%s\n' "$$device_status" | sed -n "s/.*$$field=\\([^ ]*\\).*/\\1/p"); \
		if [ -z "$$helper_value" ] || [ "$$helper_value" != "$$device_value" ]; then \
			printf 'helper target mismatch: tiny_328_tethered %s helper=%s device=%s\n' "$$field" "$$helper_value" "$$device_value"; \
			exit 1; \
		fi; \
	done; \
	printf 'helper target tiny_328_tethered %s\n' "$$helper_target"
	@helper_target=$$(printf '@target\n' | build/host/frothy-compile-overlay-tiny-host-names); \
	device_status=$$(printf 'status\n' | build/host/frothy-host-names-persist); \
	for field in profile_hash word_size int_min int_max apply_bytes; do \
		helper_value=$$(printf '%s\n' "$$helper_target" | sed -n "s/.*$$field=\\([^ ]*\\).*/\\1/p"); \
		device_value=$$(printf '%s\n' "$$device_status" | sed -n "s/.*$$field=\\([^ ]*\\).*/\\1/p"); \
		if [ -z "$$helper_value" ] || [ "$$helper_value" != "$$device_value" ]; then \
			printf 'helper target mismatch: tiny_328_tethered_host_names_persist %s helper=%s device=%s\n' "$$field" "$$helper_value" "$$device_value"; \
			exit 1; \
		fi; \
	done; \
	printf 'helper target tiny_328_tethered_host_names_persist %s\n' "$$helper_target"

test-helper-tiny-blink: frothy-session host-overlay-compiler-tiny-host-names
	$(MAKE) BOARD=host PROFILE=tiny_328_tethered_host_names_persist \
		FROTHY_BINARY=build/host/frothy-host-names-persist frothy
	@out=$$(printf '%s\n' \
		'time is 200' \
		'blink is fn [ pin: 4, 1; ms: time; pin: 4, 0; ms: time ]' \
		'blink:' \
		'14:' \
		'blink_times is fn with count [ repeat count [ blink: ] ]' \
		'blink_times: 2' \
		'blink_inline is fn with count [ repeat count [ pin: 4, 1; ms: 200; pin: 4, 0; ms: 200 ] ]' \
		'blink_inline: 2' \
		| $(FROTHY_SESSION_BINARY) --dry-run \
			--compiler build/host/frothy-compile-overlay-tiny-host-names \
		| build/host/frothy-host-names-persist); \
	ok_count=$$(printf '%s\n' "$$out" | grep -c '^> ok$$'); \
	if [ "$$ok_count" != 8 ]; then \
		printf '%s\n' "$$out"; \
		exit 1; \
	fi; \
	printf 'tiny helper blink transcript ok\n'

$(TEST_BINARY): $(TEST_DEPS)
	$(FR_CC) $(FR_CFLAGS) $(TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(UNITY_TEST_BINARY): $(UNITY_TEST_SOURCES) $(KERNEL_DEPS) $(BUILD_DEPS) \
		test/unity/unity.h test/unity/unity_internals.h | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) $(UNITY_TEST_SOURCES) $(FR_LDFLAGS) -o $@

$(OVERLAY_COMPILER): tools/frothy-compile-overlay.c $(FROTHY_DEPS) | $(BUILD_DIR)
	$(FR_CC) $(FR_CFLAGS) tools/frothy-compile-overlay.c $(KERNEL_SOURCES) $(PLATFORM_SOURCES) $(PERSISTENCE_SOURCES) $(FR_LDFLAGS) -o $@

print-config:
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

clean:
	rm -rf build frothy test/test test/test-tiny-328 test/test-tiny-328-volatile test/test-tiny-328-tethered test/test-tiny-328-tethered-host-names test/test-tiny-328-tethered-host-names-persist test/test-host-normal

.PHONY: test test-unity artifacts flash test-tiny-328 test-tiny-328-volatile test-tiny-328-tethered test-tiny-328-tethered-host-names test-tiny-328-tethered-host-names-persist test-host-normal host-normal test-host-normal-transcript test-host-normal-profile esp32-plain-host test-esp32-plain-host-transcript host-overlay-compiler host-overlay-compiler-tiny-host-names frothy-host-command frothy-session install-host test-install-host test-helper-targets print-config clean
