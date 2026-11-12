# GFX guitar effects firmware - STM32F407 Discovery (GNU make + arm-none-eabi-gcc)
#
#   make            build build/gfx.elf/.hex/.bin
#   make flash      program over SWD (ST-LINK) with STM32_Programmer_CLI
#   make clean
#
# Override on the command line when the tools live elsewhere, e.g.
#   make GCC_PATH=/opt/arm-gnu/bin PROGRAMMER=STM32_Programmer_CLI

TARGET    := gfx
BUILD_DIR := build

CUBEIDE_DIR   ?= E:/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins
TOOLCHAIN_DIR ?= $(CUBEIDE_DIR)/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin
GCC_PATH      ?= $(TOOLCHAIN_DIR)
PROGRAMMER    ?= $(CUBEIDE_DIR)/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304/tools/bin/STM32_Programmer_CLI.exe

# make V=1 shows the full command lines.
Q := $(if $(V),,@)

PREFIX  := arm-none-eabi-
TOOLBIN := $(if $(GCC_PATH),$(GCC_PATH)/)
CC      := $(TOOLBIN)$(PREFIX)gcc
OBJCOPY := $(TOOLBIN)$(PREFIX)objcopy
SIZE    := $(TOOLBIN)$(PREFIX)size

SRC_DIRS := \
  Core/Src \
  Core/Src/algos \
  USB_DEVICE/App \
  USB_DEVICE/Target \
  Drivers/STM32F4xx_HAL_Driver/Src \
  Drivers/BSP/STM32F4-Discovery \
  Drivers/BSP/Components/cs43l22 \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src \
  Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src

C_SOURCES   := $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c))
ASM_SOURCES := Core/Startup/startup_stm32f407xx.s
OBJECTS     := $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o))

INC_DIRS := \
  Core/Inc \
  USB_DEVICE/App \
  USB_DEVICE/Target \
  Drivers/STM32F4xx_HAL_Driver/Inc \
  Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
  Drivers/CMSIS/Device/ST/STM32F4xx/Include \
  Drivers/CMSIS/Include \
  Drivers/BSP/STM32F4-Discovery \
  Drivers/BSP/Components/cs43l22 \
  Drivers/BSP/Components/Common \
  Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
  Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS      := -DSTM32F407xx -DUSE_HAL_DRIVER
CFLAGS    := $(CPU_FLAGS) $(DEFS) $(addprefix -I,$(INC_DIRS)) \
             -O2 -g3 -ffunction-sections -fdata-sections -Wall -std=gnu11 \
             -MMD -MP
ASFLAGS   := $(CPU_FLAGS) -x assembler-with-cpp -g3 -MMD -MP

LDSCRIPT := STM32F407VGTX_FLASH.ld
LDFLAGS  := $(CPU_FLAGS) -T$(LDSCRIPT) -specs=nano.specs -u _printf_float \
            -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
            -Wl,--print-memory-usage
LDLIBS   := -lc -lm

.PHONY: all clean flash

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin
	@$(SIZE) $<

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	@echo LD  $@
	$(Q)$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(Q)$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(Q)$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo CC  $<
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	@echo AS  $<
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

flash: $(BUILD_DIR)/$(TARGET).elf
	$(PROGRAMMER) -c port=SWD -w $< -v -rst

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
