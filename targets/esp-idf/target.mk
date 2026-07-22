TARGET_MAIN_SOURCE := targets/esp-idf/main/main.c
TARGET_SOURCES += \
	targets/common/target_defs.c \
	targets/esp-idf/ble.c \
	targets/esp-idf/platform.c
BUILD_DIR ?= build/$(BOARD)

ESP_IDF_PROFILE_STAMP := $(BUILD_DIR)/.frothy-profile-$(PROFILE)

# Key the sdkconfig stamp by composition state so a default -> composed ->
# default transition always invalidates. A constant-named stamp would stay
# current when the overlay leaves the prerequisite list, silently keeping the
# composed sdkconfig. The empty-guard matters: `shasum` with no file argument
# reads stdin and would hang.
ifeq ($(strip $(FROTHY_COMPOSITION_SDKCONFIG)),)
ESP_IDF_COMPOSITION_KEY := default
else
ESP_IDF_COMPOSITION_KEY := $(firstword $(shell shasum -a 256 "$(FROTHY_COMPOSITION_SDKCONFIG)" 2>/dev/null || sha256sum "$(FROTHY_COMPOSITION_SDKCONFIG)" 2>/dev/null))
endif
ESP_IDF_SDKCONFIG_STAMP := $(BUILD_DIR)/.frothy-sdkconfig-$(ESP_IDF_COMPOSITION_KEY)
ESP_IDF_SDKCONFIG_DEFAULTS := \
	targets/esp-idf/sdkconfig.defaults \
	profiles/$(PROFILE).sdkconfig.defaults \
	$(wildcard profiles/$(PROFILE).sdkconfig.defaults.$(BOARD_ESP_IDF_TARGET)) \
	$(FROTHY_COMPOSITION_SDKCONFIG)

TARGET_BUILD_DEPS += \
	targets/esp-idf/CMakeLists.txt \
	targets/esp-idf/main/CMakeLists.txt \
	$(FROTHY_COMPOSITION_H) \
	$(ESP_IDF_SDKCONFIG_STAMP)

ESP_IDF_PROJECT_DIR := targets/esp-idf
ESP_IDF_BUILD_DIR = $(abspath $(BUILD_DIR))
ESP_IDF_SDKCONFIG = $(ESP_IDF_BUILD_DIR)/sdkconfig
ESP_IDF_ROOT := $(abspath .)
ESP_IDF_BOARD_TARGET ?= $(BOARD_ESP_IDF_TARGET)
ESP_IDF_ASSERT_CUSTOM_PARTITION_TABLE = if ! { [ -f "$(ESP_IDF_SDKCONFIG)" ] && grep -qx 'CONFIG_PARTITION_TABLE_CUSTOM=y' "$(ESP_IDF_SDKCONFIG)"; }; then printf 'stale ESP-IDF build cache: partition table is not the custom one; run rm -rf $(BUILD_DIR) and rebuild\n'; exit 2; fi

# frothy build passes these so main/CMakeLists.txt can include the generator
# output and the composition overrides. Empty library strings resolve to the
# weak fr_lib_natives defaults; empty composition paths are treated as unset on
# the CMake side. Not library-only anymore, hence the rename.
ESP_IDF_FROTHY_GEN_DEFINES = -DFROTHY_LIBS_CMAKE="$(FROTHY_LIBS_CMAKE)" -DFROTHY_LIB_NATIVES_C="$(FROTHY_LIB_NATIVES_C)" -DFR_COMPOSITION_H="$(abspath $(FROTHY_COMPOSITION_H))" -DFR_COMPOSITION_SDKCONFIG="$(abspath $(FROTHY_COMPOSITION_SDKCONFIG))"
TARGET_ARTIFACTS_CHECK = @$(ESP_IDF_ASSERT_CUSTOM_PARTITION_TABLE)
TARGET_FLASH_CHECK = @$(ESP_IDF_ASSERT_CUSTOM_PARTITION_TABLE)

# The profile-named stamp makes a PROFILE switch visible to Make. Keep only the
# current one so switching back is visible too.
$(ESP_IDF_PROFILE_STAMP): | $(BUILD_DIR)
	$(RM) "$(BUILD_DIR)"/.frothy-profile-*
	touch "$@"

# Recreate generated sdkconfig only when its checked-in inputs or profile
# changed. Ordinary source edits retain ESP-IDF's incremental build path.
$(ESP_IDF_SDKCONFIG_STAMP): $(ESP_IDF_SDKCONFIG_DEFAULTS) $(ESP_IDF_PROFILE_STAMP) | $(BUILD_DIR)
	$(RM) "$(BUILD_DIR)"/.frothy-sdkconfig-*
	$(RM) "$(ESP_IDF_SDKCONFIG)"
	touch "$@"

TARGET_BUILD_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" -DFR_REWRITE_ROOT="$(ESP_IDF_ROOT)" -DFR_REWRITE_BOARD="$(BOARD)" -DFR_REWRITE_PROFILE="$(PROFILE)" -DIDF_TARGET="$(ESP_IDF_BOARD_TARGET)" $(ESP_IDF_FROTHY_GEN_DEFINES) build
# Size reporting is entirely best-effort: stale outputs are removed before
# either command runs, so a failure at any point leaves no report (the CLI
# treats a missing frothy.size.json as "skip") and never fails a build whose
# firmware already exists.
TARGET_SIZE_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && rm -f "$(abspath $(ARTIFACT_SIZE))" "$(abspath $(ARTIFACT_SIZE)).json" "$(abspath $(ARTIFACT_SIZE)).json.tmp" && { . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" size > "$(abspath $(ARTIFACT_SIZE))" && python -m esp_idf_size --format json "$(ESP_IDF_BUILD_DIR)/frothy.map" > "$(abspath $(ARTIFACT_SIZE)).json.tmp" && mv "$(abspath $(ARTIFACT_SIZE)).json.tmp" "$(abspath $(ARTIFACT_SIZE)).json"; } || true
TARGET_FLASH_DEPS = $(ARTIFACT_ELF)
TARGET_FLASH_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" $(ESP_IDF_FROTHY_GEN_DEFINES) -p "$(BOARD_PORT)" flash
