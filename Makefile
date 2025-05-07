# AymOS Makefile
PROJECT = aymos

# Compiler settings
PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc
AS = $(PREFIX)as
LD = $(PREFIX)ld
OBJCOPY = $(PREFIX)objcopy
OBJDUMP = $(PREFIX)objdump
SIZE = $(PREFIX)size

# Target MCU
MCU = cortex-m4
FPU = -mfloat-abi=hard -mfpu=fpv4-sp-d16

# Directories
SRC_DIR = Src
INC_DIR = Inc
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
ASMS = $(wildcard $(SRC_DIR)/*.s)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o) $(ASMS:$(SRC_DIR)/%.s=$(OBJ_DIR)/%.o)

# Include directories
INCLUDES = -I$(INC_DIR)

# Compiler flags
CFLAGS = -mcpu=$(MCU) $(FPU) -mthumb -Wall -g3 -O0 \
         -ffunction-sections -fdata-sections \
         -fno-strict-aliasing -fno-builtin -fshort-enums \
         $(INCLUDES)

# Linker flags
LDFLAGS = -mcpu=$(MCU) $(FPU) -mthumb -specs=nano.specs \
          -TSTM32F407VGTx_FLASH.ld \
          -Wl,-Map=$(BIN_DIR)/$(PROJECT).map \
          -Wl,--gc-sections

# Default target
all: directories $(BIN_DIR)/$(PROJECT).elf $(BIN_DIR)/$(PROJECT).hex $(BIN_DIR)/$(PROJECT).bin

# Create necessary directories
directories:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

# Compile C source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile assembly files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.s
	@echo "Assembling $<"
	@$(AS) -mcpu=$(MCU) -mthumb $< -o $@

# Link object files
$(BIN_DIR)/$(PROJECT).elf: $(OBJS)
	@echo "Linking $@"
	@$(CC) $(LDFLAGS) $(OBJS) -o $@
	@$(SIZE) $@

# Create hex file
$(BIN_DIR)/$(PROJECT).hex: $(BIN_DIR)/$(PROJECT).elf
	@echo "Creating hex file"
	@$(OBJCOPY) -O ihex $< $@

# Create binary file
$(BIN_DIR)/$(PROJECT).bin: $(BIN_DIR)/$(PROJECT).elf
	@echo "Creating binary file"
	@$(OBJCOPY) -O binary $< $@

# Clean build files
clean:
	@echo "Cleaning build files"
	@rm -rf $(BUILD_DIR)

# Flash the program (requires ST-Link)
flash: $(BIN_DIR)/$(PROJECT).elf
	@echo "Flashing program"
	@st-flash write $(BIN_DIR)/$(PROJECT).bin 0x8000000

# Debug information
debug: $(BIN_DIR)/$(PROJECT).elf
	@echo "Generating debug information"
	@$(OBJDUMP) -S $< > $(BIN_DIR)/$(PROJECT).lst

# Help information
help:
	@echo "Available targets:"
	@echo "  all      - Build the project (default)"
	@echo "  clean    - Remove all build files"
	@echo "  flash    - Flash the program to the device"
	@echo "  debug    - Generate debug information"
	@echo "  help     - Show this help message"

.PHONY: all clean flash debug help directories 