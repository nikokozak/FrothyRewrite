AVR_GCC ?= $(firstword $(wildcard /opt/homebrew/opt/avr-gcc@*/bin/avr-gcc) avr-gcc)
AVR_OBJCOPY ?= $(firstword $(wildcard /opt/homebrew/opt/avr-binutils/bin/avr-objcopy) avr-objcopy)
AVR_SIZE ?= $(firstword $(wildcard /opt/homebrew/opt/avr-binutils/bin/avr-size) avr-size)
AVRDUDE ?= $(firstword $(wildcard /opt/homebrew/bin/avrdude /usr/local/bin/avrdude) avrdude)
TARGET_CC ?= $(AVR_GCC)
TARGET_OBJCOPY ?= $(AVR_OBJCOPY)
TARGET_SIZE ?= $(AVR_SIZE)
AVR_SIZE_CFLAGS ?= -ffunction-sections -fdata-sections -flto
AVR_SIZE_LDFLAGS ?= -flto -Wl,--gc-sections -Wl,--relax
TARGET_CFLAGS += -mmcu=$(BOARD_MCU) -Os -DF_CPU=$(BOARD_F_CPU)UL $(AVR_SIZE_CFLAGS)
TARGET_LDFLAGS += -mmcu=$(BOARD_MCU) $(AVR_SIZE_LDFLAGS)
TARGET_MAP_LDFLAG = -Wl,-Map,$(ARTIFACT_MAP)
TARGET_FLASH_COMMAND = $(AVRDUDE) -p $(BOARD_MCU) -c $(BOARD_PROGRAMMER) -P $(BOARD_PORT) -b $(BOARD_UPLOAD_BAUD) -D -U flash:w:$(ARTIFACT_HEX):i
TARGET_SOURCES += targets/common/target_defs.c targets/avr-libc/platform.c
