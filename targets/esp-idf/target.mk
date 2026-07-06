TARGET_MAIN_SOURCE := targets/esp-idf/main/main.c
TARGET_SOURCES += targets/common/target_defs.c targets/esp-idf/platform.c
TARGET_BUILD_DEPS += \
	targets/esp-idf/CMakeLists.txt \
	targets/esp-idf/main/CMakeLists.txt \
	targets/esp-idf/sdkconfig.defaults

ESP_IDF_PROJECT_DIR := targets/esp-idf
ESP_IDF_BUILD_DIR = $(abspath $(BUILD_DIR))
ESP_IDF_SDKCONFIG = $(ESP_IDF_BUILD_DIR)/sdkconfig
ESP_IDF_ROOT := $(abspath .)
ESP_IDF_BOARD_TARGET ?= $(BOARD_ESP_IDF_TARGET)

# T12L: frothy build passes these so main/CMakeLists.txt can include
# the generator output. Empty strings mean "no libraries", which
# resolves to the weak fr_lib_natives defaults in src/lib_native.c.
ESP_IDF_FROTHY_LIB_DEFINES = -DFROTHY_LIBS_CMAKE="$(FROTHY_LIBS_CMAKE)" -DFROTHY_LIB_NATIVES_C="$(FROTHY_LIB_NATIVES_C)"

TARGET_BUILD_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null 2>&1 && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" -DFR_REWRITE_ROOT="$(ESP_IDF_ROOT)" -DFR_REWRITE_BOARD="$(BOARD)" -DFR_REWRITE_PROFILE="$(PROFILE)" -DIDF_TARGET="$(ESP_IDF_BOARD_TARGET)" $(ESP_IDF_FROTHY_LIB_DEFINES) build
TARGET_SIZE_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null 2>&1 && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" size > "$(abspath $(ARTIFACT_SIZE))"
TARGET_FLASH_DEPS = $(ARTIFACT_ELF)
TARGET_FLASH_COMMAND = cd $(ESP_IDF_PROJECT_DIR) && . "$$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null 2>&1 && idf.py -B "$(ESP_IDF_BUILD_DIR)" -DSDKCONFIG="$(ESP_IDF_SDKCONFIG)" -p "$(BOARD_PORT)" flash
