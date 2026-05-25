ROOT := $(abspath $(CURDIR)/../..)
SDK_PATH := $(ROOT)/bthome_phy6222/SDK
BUILD_DIR ?= $(ROOT)/inkbird_fw/build/stock_bundle_installer
TARGET ?= STOCK_BUNDLE_INSTALLER
STAGING_ADDR ?= 0x1106E000
TRACE_ADDR ?= 0x1107B000
TRACE_UART ?= 0
TRACE_UART_BAUD ?= 9600

CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
SIZE := $(CROSS_COMPILE)size

CFLAGS := -mcpu=cortex-m0 -mthumb -Os -g3 -ffunction-sections -fdata-sections
CFLAGS += -fno-builtin -fno-common -Wall -Wextra -std=c11
CFLAGS += -DIBI3_STAGING_ADDR=$(STAGING_ADDR)
CFLAGS += -DINSTALLER_TRACE_ADDR=$(TRACE_ADDR)
CFLAGS += -DINSTALLER_UART_TRACE=$(TRACE_UART) -DINSTALLER_UART_BAUD=$(TRACE_UART_BAUD)
ASFLAGS := -mcpu=cortex-m0 -mthumb -g3
LDFLAGS := -mcpu=cortex-m0 -mthumb -nostdlib -nostartfiles
LDFLAGS += -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += -Wl,--script=$(CURDIR)/installer.ld
LDFLAGS += -Wl,--just-symbols=$(SDK_PATH)/misc/bb_rom_sym_m0.gcc

OBJS := $(BUILD_DIR)/start.o $(BUILD_DIR)/installer.o

.PHONY: all clean

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).asm
	$(SIZE) $(BUILD_DIR)/$(TARGET).elf

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/installer.o: installer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) installer.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).asm: $(BUILD_DIR)/$(TARGET).elf
	$(OBJDUMP) -d -S $< > $@

clean:
	rm -rf $(BUILD_DIR)
