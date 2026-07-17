TARGET_MAIN_SOURCE := targets/esp-idf/main/main.c
TARGET_SOURCES += \
	targets/common/target_defs.c \
	targets/esp-idf/ble.c \
	targets/esp-idf/platform.c
BUILD_DIR ?= build/$(BOARD)

ESP_IDF_PROFILE_STAMP := $(BUILD_DIR)/.frothy-profile-$(PROFILE)
ESP_IDF_SDKCONFIG_STAMP := $(BUILD_DIR)/.frothy-sdkconfig-defaults
ESP_IDF_SDKCONFIG_DEFAULTS := \
	targets/esp-idf/sdkconfig.defaults \
	profiles/$(PROFILE).sdkconfig.defaults

TARGET_BUILD_DEPS += \
	targets/esp-idf/CMakeLists.txt \
	targets/esp-idf/main/CMakeLists.txt \
	$(ESP_IDF_SDKCONFIG_STAMP)

ESP_IDF_PROJECT_DIR := targets/esp-idf
ESP_IDF_BUILD_DIR = $(abspath $(BUILD_DIR))
ESP_IDF_SDKCONFIG = $(ESP_IDF_BUILD_DIR)/sdkconfig
ESP_IDF_ROOT := $(abspath .)
ESP_IDF_BOARD_TARGET ?= $(BOARD_ESP_IDF_TARGET)
ESP_IDF_ASSERT_CUSTOM_PARTITION_TABLE = if ! { [ -f "$(ESP_IDF_SDKCONFIG)" ] && grep -qx 'CONFIG_PARTITION_TABLE_CUSTOM=y' "$(ESP_IDF_SDKCONFIG)"; }; then printf 'stale ESP-IDF build cache: partition table is not the custom one; run rm -rf $(BUILD_DIR) and rebuild\n'; exit 2; fi

# T12L: frothy build passes these so main/CMakeLists.txt can include
# the generator output. Empty strings mean "no libraries", which
# resolves to the weak fr_lib_natives defaults in src/lib_native.c.
ESP_IDF_FROTHY_LIB_DEFINES = -DFROTHY_LIBS_CMAKE="$(FROTHY_LIBS_CMAKE)" -DFROTHY_LIB_NATIVES_C="$(FROTHY_LIB_NATIVES_C)"
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
	$(RM) "$(ESP_IDF_SDKCONFIG)"
	touch "$@"

TARGET_BUILD_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" -DFR_REWRITE_ROOT="$(ESP_IDF_ROOT)" -DFR_REWRITE_BOARD="$(BOARD)" -DFR_REWRITE_PROFILE="$(PROFILE)" -DIDF_TARGET="$(ESP_IDF_BOARD_TARGET)" $(ESP_IDF_FROTHY_LIB_DEFINES) build
TARGET_SIZE_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" size > "$(abspath $(ARTIFACT_SIZE))"
TARGET_FLASH_DEPS = $(ARTIFACT_ELF)
TARGET_FLASH_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" -p "$(BOARD_PORT)" flash
