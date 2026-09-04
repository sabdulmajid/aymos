SHELL := /bin/bash
export LC_ALL := C
.DEFAULT_GOAL := firmware

PROJECT := aymos
BOARD ?= nucleo_f401re
APP ?= boot
WORKLOAD_MODE ?= normal
SIGNAL_IMPL ?= scalar

SUPPORTED_BOARDS := nucleo_f401re
SUPPORTED_APPS := boot lifecycle edf allocator trace deadline_lab signal_lab
SUPPORTED_WORKLOAD_MODES := normal overload
SUPPORTED_SIGNAL_IMPLS := scalar m4

ifeq ($(filter $(BOARD),$(SUPPORTED_BOARDS)),)
$(error Unsupported BOARD '$(BOARD)'; supported boards: $(SUPPORTED_BOARDS))
endif
ifeq ($(APP),signal_lab)
ifeq ($(filter $(SIGNAL_IMPL),$(SUPPORTED_SIGNAL_IMPLS)),)
$(error Unsupported SIGNAL_IMPL '$(SIGNAL_IMPL)'; supported implementations: $(SUPPORTED_SIGNAL_IMPLS))
endif
endif
ifeq ($(filter $(APP),$(SUPPORTED_APPS)),)
$(error Unsupported APP '$(APP)'; supported applications: $(SUPPORTED_APPS))
endif
ifeq ($(APP),deadline_lab)
ifeq ($(filter $(WORKLOAD_MODE),$(SUPPORTED_WORKLOAD_MODES)),)
$(error Unsupported WORKLOAD_MODE '$(WORKLOAD_MODE)'; supported modes: $(SUPPORTED_WORKLOAD_MODES))
endif
endif

# PR 1 supports only the locked project-local toolchain. Reject external tool
# selection before assigning these variables so command-line and environment
# attempts cannot bypass tools/setup.sh --check. CC's built-in default is safe
# to replace; all other non-default definitions are external overrides.
override LOCKED_TOOL_VARIABLES := \
	ARM_GNU_DIR \
	CROSS_COMPILE \
	CC \
	OBJCOPY \
	OBJDUMP \
	READELF \
	SIZE \
	NM \
	RENODE_DIR \
	RENODE \
	PYTHON_ENV_DIR \
	PYTHON
override EXTERNAL_TOOL_VARIABLES := $(strip $(foreach variable,$(LOCKED_TOOL_VARIABLES),\
	$(if $(filter undefined default,$(origin $(variable))),,\
		$(variable)[$(origin $(variable))])))
ifneq ($(EXTERNAL_TOOL_VARIABLES),)
$(error Locked tool variable override(s) rejected: $(EXTERNAL_TOOL_VARIABLES). Remove them and run 'make setup')
endif

override ARM_GNU_DIR := .tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
override CROSS_COMPILE := $(abspath $(ARM_GNU_DIR))/bin/arm-none-eabi-
override CC := $(CROSS_COMPILE)gcc
override OBJCOPY := $(CROSS_COMPILE)objcopy
override OBJDUMP := $(CROSS_COMPILE)objdump
override READELF := $(CROSS_COMPILE)readelf
override SIZE := $(CROSS_COMPILE)size
override NM := $(CROSS_COMPILE)nm
override RENODE_DIR := $(abspath .tools/renode-1.16.1-dotnet-x86_64)
override RENODE := $(RENODE_DIR)/renode
override PYTHON_ENV_DIR := $(abspath .tools/python-venv-renode-1.16.1)
override PYTHON := $(PYTHON_ENV_DIR)/bin/python3
override HOST_CC := cc

CMSIS_CORE_DIR := .deps/stm32cube_f4_core/Drivers/CMSIS/Core/Include
CMSIS_DEVICE_DIR := .deps/cmsis_device_f4
HAL_DIR := .deps/stm32f4xx_hal_driver

ifeq ($(APP),deadline_lab)
BUILD_DIR := build/$(BOARD)/$(APP)/$(WORKLOAD_MODE)
else ifeq ($(APP),signal_lab)
BUILD_DIR := build/$(BOARD)/$(APP)/$(SIGNAL_IMPL)
else
BUILD_DIR := build/$(BOARD)/$(APP)
endif
OBJ_DIR := $(BUILD_DIR)/obj
ELF := $(BUILD_DIR)/$(PROJECT).elf
BIN := $(BUILD_DIR)/$(PROJECT).bin
MAP := $(BUILD_DIR)/$(PROJECT).map
SIZE_REPORT := $(BUILD_DIR)/size.txt
BUILD_METADATA := $(BUILD_DIR)/build-metadata.txt
LINKER_SCRIPT := bsp/nucleo_f401re/stm32f401re.ld

COMMON_PROJECT_C_SOURCES := \
	bsp/nucleo_f401re/src/board.c \
	bsp/nucleo_f401re/src/newlib_heap.c
DSP_PROJECT_C_SOURCES :=

ifeq ($(APP),boot)
PROJECT_C_SOURCES := \
	apps/boot/main.c \
	bsp/nucleo_f401re/src/interrupts.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES :=
else ifeq ($(APP),lifecycle)
PROJECT_C_SOURCES := \
	apps/lifecycle/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S \
	apps/lifecycle/register_probe.S
else ifeq ($(APP),edf)
PROJECT_C_SOURCES := \
	apps/edf/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S
else ifeq ($(APP),allocator)
PROJECT_C_SOURCES := \
	apps/allocator/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S
else ifeq ($(APP),trace)
PROJECT_C_SOURCES := \
	apps/trace/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	kernel/src/trace.c \
	kernel/src/trace_runtime.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S
PROJECT_CPPFLAGS := -DAYMOS_TRACE_ENABLED=1
else ifeq ($(APP),deadline_lab)
PROJECT_C_SOURCES := \
	apps/deadline_lab/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	kernel/src/trace.c \
	kernel/src/trace_runtime.c \
	$(COMMON_PROJECT_C_SOURCES)
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S
PROJECT_CPPFLAGS := \
	-DAYMOS_TRACE_ENABLED=1 \
	-DAYMOS_DEADLINE_LAB_OVERLOAD=$(if $(filter overload,$(WORKLOAD_MODE)),1,0)
else ifeq ($(APP),signal_lab)
PROJECT_C_SOURCES := \
	apps/signal_lab/main.c \
	bsp/nucleo_f401re/src/kernel_interrupts.c \
	kernel/src/allocator.c \
	kernel/src/kernel.c \
	kernel/src/scheduler.c \
	kernel/src/trace.c \
	kernel/src/trace_runtime.c \
	$(COMMON_PROJECT_C_SOURCES)
DSP_PROJECT_C_SOURCES := \
	dsp/src/fir_q15_common.c \
	dsp/src/fir_q15_$(SIGNAL_IMPL).c
PROJECT_ASM_SOURCES := \
	arch/arm_cm4/context_switch.S
PROJECT_CPPFLAGS := \
	-DAYMOS_TRACE_ENABLED=1 \
	-DAYMOS_SIGNAL_IMPL_M4=$(if $(filter m4,$(SIGNAL_IMPL)),1,0) \
	-Idsp/include \
	-Idsp/src
endif

VENDOR_C_SOURCES := \
	$(CMSIS_DEVICE_DIR)/Source/Templates/system_stm32f4xx.c \
	$(HAL_DIR)/Src/stm32f4xx_hal.c \
	$(HAL_DIR)/Src/stm32f4xx_hal_cortex.c \
	$(HAL_DIR)/Src/stm32f4xx_hal_flash.c \
	$(HAL_DIR)/Src/stm32f4xx_hal_gpio.c \
	$(HAL_DIR)/Src/stm32f4xx_hal_rcc.c \
	$(HAL_DIR)/Src/stm32f4xx_hal_uart.c

VENDOR_ASM_SOURCES := \
	$(CMSIS_DEVICE_DIR)/Source/Templates/gcc/startup_stm32f401xe.s

PROJECT_OBJECTS := $(addprefix $(OBJ_DIR)/,$(PROJECT_C_SOURCES:.c=.o))
DSP_PROJECT_OBJECTS := $(addprefix $(OBJ_DIR)/,$(DSP_PROJECT_C_SOURCES:.c=.o))
PROJECT_ASM_OBJECTS := $(addprefix $(OBJ_DIR)/,$(PROJECT_ASM_SOURCES:.S=.o))
VENDOR_C_OBJECTS := $(addprefix $(OBJ_DIR)/,$(VENDOR_C_SOURCES:.c=.o))
VENDOR_ASM_OBJECTS := $(addprefix $(OBJ_DIR)/,$(VENDOR_ASM_SOURCES:.s=.o))
OBJECTS := $(VENDOR_ASM_OBJECTS) $(PROJECT_OBJECTS) $(DSP_PROJECT_OBJECTS) \
	$(PROJECT_ASM_OBJECTS) $(VENDOR_C_OBJECTS)
DEPENDENCY_FILES := $(OBJECTS:.o=.d)

ARCH_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
COMMON_CPPFLAGS := \
	-DSTM32F401xE \
	-DUSE_HAL_DRIVER \
	-DUSER_VECT_TAB_ADDRESS \
	-Ibsp/nucleo_f401re/include \
	-Ikernel/include \
	-isystem $(CMSIS_CORE_DIR) \
	-isystem $(CMSIS_DEVICE_DIR)/Include \
	-isystem $(HAL_DIR)/Inc \
	-isystem $(HAL_DIR)/Inc/Legacy
COMMON_CFLAGS := \
	$(ARCH_FLAGS) \
	-std=c11 \
	-ffreestanding \
	-ffunction-sections \
	-fdata-sections \
	-fno-builtin \
	-fno-common \
	-fno-strict-aliasing \
	-g3 \
	-Og
override DSP_FIRMWARE_CFLAGS := $(filter-out -Og,$(COMMON_CFLAGS)) -O2
PROJECT_WARNINGS := \
	-Wall \
	-Wextra \
	-Werror \
	-Wformat=2 \
	-Wmissing-prototypes \
	-Wpointer-arith \
	-Wshadow \
	-Wstrict-prototypes \
	-Wundef
VENDOR_WARNINGS := -Wall -Wextra
DEPENDENCY_FLAGS := -MMD -MP

NATIVE_BUILD_DIR := build/native
NATIVE_SCHEDULER_TEST := $(NATIVE_BUILD_DIR)/test_scheduler
NATIVE_COMPILER_REPORT := $(NATIVE_BUILD_DIR)/compiler.txt
NATIVE_TEST_SOURCES := kernel/src/scheduler.c tests/native/test_scheduler.c
NATIVE_ALLOCATOR_TEST := $(NATIVE_BUILD_DIR)/test_allocator
NATIVE_ALLOCATOR_COMPILER_REPORT := $(NATIVE_BUILD_DIR)/compiler-allocator.txt
NATIVE_ALLOCATOR_TEST_SOURCES := \
	kernel/src/allocator.c \
	tests/native/test_allocator.c
NATIVE_TRACE_TEST := $(NATIVE_BUILD_DIR)/test_trace
NATIVE_TRACE_COMPILER_REPORT := $(NATIVE_BUILD_DIR)/compiler-trace.txt
NATIVE_TRACE_TEST_SOURCES := kernel/src/trace.c tests/native/test_trace.c
NATIVE_DSP_TEST := $(NATIVE_BUILD_DIR)/test_fir_q15
NATIVE_DSP_COMPILER_REPORT := $(NATIVE_BUILD_DIR)/compiler-fir-q15.txt
NATIVE_DSP_TEST_SOURCES := \
	dsp/src/fir_q15_common.c \
	dsp/src/fir_q15_scalar.c \
	dsp/src/fir_q15_packed_portable.c \
	tests/native/test_fir_q15.c
NATIVE_CFLAGS := \
	-std=c11 \
	-O1 \
	-g3 \
	-Wall \
	-Wextra \
	-Werror \
	-Wconversion \
	-Wshadow \
	-Wstrict-prototypes \
	-Wundef \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer \
	-Ikernel/include
NATIVE_DSP_CFLAGS := \
	-std=c11 \
	-O2 \
	-g3 \
	-Wall \
	-Wextra \
	-Werror \
	-Wconversion \
	-Wmissing-prototypes \
	-Wpointer-arith \
	-Wshadow \
	-Wstrict-prototypes \
	-Wundef \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer \
	-Idsp/include \
	-Idsp/src

DSP_CODEGEN_BUILD_DIR := build/dsp-codegen
DSP_CODEGEN_COMMON_OBJECT := $(DSP_CODEGEN_BUILD_DIR)/fir_q15_common.o
DSP_CODEGEN_M4_OBJECT := $(DSP_CODEGEN_BUILD_DIR)/fir_q15_m4.o
DSP_CODEGEN_PACKED_OBJECT := $(DSP_CODEGEN_BUILD_DIR)/fir_q15_packed_portable.o
DSP_CODEGEN_SCALAR_OBJECT := $(DSP_CODEGEN_BUILD_DIR)/fir_q15_scalar.o
DSP_CODEGEN_CONFIG := $(DSP_CODEGEN_BUILD_DIR)/build-config.txt
DSP_CODEGEN_REPORT := $(DSP_CODEGEN_BUILD_DIR)/codegen-report.json
DSP_ARM_CFLAGS := \
	$(ARCH_FLAGS) \
	-std=c11 \
	-ffreestanding \
	-ffunction-sections \
	-fdata-sections \
	-fno-builtin \
	-fno-common \
	-fno-strict-aliasing \
	-O2 \
	$(PROJECT_WARNINGS) \
	-Wconversion \
	-Idsp/include \
	-Idsp/src \
	-isystem $(CMSIS_CORE_DIR)

TRACE_INPUT ?= uart.bin
TRACE_JSON ?= trace.json
TRACE_BIN ?= trace.bin

LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostartfiles \
	--specs=nano.specs \
	-T$(LINKER_SCRIPT) \
	-Wl,--fatal-warnings \
	-Wl,--gc-sections \
	-Wl,--print-memory-usage \
	-Wl,-Map,$(MAP) \
	-Wl,--cref

.PHONY: firmware lifecycle edf allocator trace workload deadline-lab dsp \
	signal-lab quickstart demo demo-dsp report-dsp setup validate test test-native \
	test-native-scheduler test-native-allocator test-native-trace test-native-dsp \
	check-dsp-codegen check-dsp-codegen-internal run run-lifecycle run-edf \
	run-allocator run-trace test-emulator test-emulator-offline test-lifecycle test-edf \
	test-allocator test-trace test-dsp test-signal-lab test-host-trace \
	test-host-scheduling test-host-deadline-lab test-host-dsp \
	test-host-signal-lab test-host-performance-report run-dsp run-signal-lab \
	decode-trace \
	check-renode-platform clean clean-build clean-emulator flash disassembly \
	help check-setup FORCE

firmware: check-setup $(ELF) $(BIN) $(SIZE_REPORT) $(BUILD_METADATA) validate

setup:
	@./tools/setup.sh

check-setup:
	@./tools/setup.sh --check

check-renode-platform:
	@./tools/renode/check_platform.sh

test: test-native test-host-trace test-host-scheduling test-host-dsp \
	test-host-performance-report check-setup \
	check-renode-platform
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m unittest discover -s tests/renode -p 'test_*.py' -v

test-host-trace: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m unittest tests.host.test_trace_decoder \
			tests.host.test_trace_workload -v

test-host-scheduling: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m unittest tests.host.test_deadline_lab -v

test-host-deadline-lab: test-host-scheduling

test-host-dsp: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m unittest tests.host.test_signal_lab -v

test-host-signal-lab: test-host-dsp

test-host-performance-report: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m unittest tests.host.test_performance_report -v

decode-trace: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m tools.aymos_lab.trace "$(TRACE_INPUT)" \
			--json "$(TRACE_JSON)" --trace-bin "$(TRACE_BIN)"

test-native: test-native-scheduler test-native-allocator test-native-trace \
	test-native-dsp

test-native-scheduler: $(NATIVE_TEST_SOURCES) kernel/include/aymos_scheduler.h \
	kernel/include/aymos_edf_fixture.h FORCE
	@mkdir -p "$(NATIVE_BUILD_DIR)"
	@printf 'HOSTCC      %s\n' "$(NATIVE_SCHEDULER_TEST)"
	@set -eu; \
		host_cc="$$(command -v $(HOST_CC))"; \
		host_cc_real="$$(readlink -f "$${host_cc}")"; \
		test_tmp='$(NATIVE_SCHEDULER_TEST).tmp'; \
		report_tmp='$(NATIVE_COMPILER_REPORT).tmp'; \
		trap 'rm -f -- "$${test_tmp}" "$${report_tmp}"' EXIT; \
		{ \
			printf 'schema=1\n'; \
			printf 'compiler_command=%s\n' '$(HOST_CC)'; \
			printf 'compiler_path=%s\n' "$${host_cc}"; \
			printf 'compiler_realpath=%s\n' "$${host_cc_real}"; \
			printf 'compiler_sha256=%s\n' \
				"$$(sha256sum "$${host_cc_real}" | awk '{print $$1}')"; \
			printf 'flags=%s\n' '$(NATIVE_CFLAGS)'; \
			printf '%s\n' 'compiler_version_begin'; \
			"$${host_cc_real}" --version; \
			printf '%s\n' 'compiler_version_end'; \
			sha256sum $(NATIVE_TEST_SOURCES) \
				kernel/include/aymos_scheduler.h \
				kernel/include/aymos_edf_fixture.h; \
		} > "$${report_tmp}"; \
		"$${host_cc_real}" $(NATIVE_CFLAGS) $(NATIVE_TEST_SOURCES) \
			-o "$${test_tmp}"; \
		printf 'binary_sha256=%s\n' \
			"$$(sha256sum "$${test_tmp}" | awk '{print $$1}')" \
			>> "$${report_tmp}"; \
		mv -- "$${test_tmp}" '$(NATIVE_SCHEDULER_TEST)'; \
		mv -- "$${report_tmp}" '$(NATIVE_COMPILER_REPORT)'; \
		trap - EXIT; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			'$(NATIVE_SCHEDULER_TEST)'; \
		expected="$$(awk -F= '/^binary_sha256=/ {print $$2}' \
			'$(NATIVE_COMPILER_REPORT)')"; \
		actual="$$(sha256sum '$(NATIVE_SCHEDULER_TEST)' | awk '{print $$1}')"; \
		test "$${actual}" = "$${expected}"

test-native-allocator: $(NATIVE_ALLOCATOR_TEST_SOURCES) \
	kernel/include/aymos_allocator.h FORCE
	@mkdir -p "$(NATIVE_BUILD_DIR)"
	@printf 'HOSTCC      %s\n' "$(NATIVE_ALLOCATOR_TEST)"
	@set -eu; \
		host_cc="$$(command -v $(HOST_CC))"; \
		host_cc_real="$$(readlink -f "$${host_cc}")"; \
		test_tmp='$(NATIVE_ALLOCATOR_TEST).tmp'; \
		report_tmp='$(NATIVE_ALLOCATOR_COMPILER_REPORT).tmp'; \
		trap 'rm -f -- "$${test_tmp}" "$${report_tmp}"' EXIT; \
		{ \
			printf 'schema=1\n'; \
			printf 'suite=allocator\n'; \
			printf 'compiler_command=%s\n' '$(HOST_CC)'; \
			printf 'compiler_path=%s\n' "$${host_cc}"; \
			printf 'compiler_realpath=%s\n' "$${host_cc_real}"; \
			printf 'compiler_sha256=%s\n' \
				"$$(sha256sum "$${host_cc_real}" | awk '{print $$1}')"; \
			printf 'flags=%s\n' '$(NATIVE_CFLAGS)'; \
			printf '%s\n' 'compiler_version_begin'; \
			"$${host_cc_real}" --version; \
			printf '%s\n' 'compiler_version_end'; \
			sha256sum $(NATIVE_ALLOCATOR_TEST_SOURCES) \
				kernel/include/aymos_allocator.h; \
		} > "$${report_tmp}"; \
		"$${host_cc_real}" $(NATIVE_CFLAGS) \
			$(NATIVE_ALLOCATOR_TEST_SOURCES) -o "$${test_tmp}"; \
		printf 'binary_sha256=%s\n' \
			"$$(sha256sum "$${test_tmp}" | awk '{print $$1}')" \
			>> "$${report_tmp}"; \
		mv -- "$${test_tmp}" '$(NATIVE_ALLOCATOR_TEST)'; \
		mv -- "$${report_tmp}" '$(NATIVE_ALLOCATOR_COMPILER_REPORT)'; \
		trap - EXIT; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			'$(NATIVE_ALLOCATOR_TEST)'; \
		expected="$$(awk -F= '/^binary_sha256=/ {print $$2}' \
			'$(NATIVE_ALLOCATOR_COMPILER_REPORT)')"; \
		actual="$$(sha256sum '$(NATIVE_ALLOCATOR_TEST)' | awk '{print $$1}')"; \
		test "$${actual}" = "$${expected}"

test-native-trace: $(NATIVE_TRACE_TEST_SOURCES) kernel/include/aymos_trace.h FORCE
	@mkdir -p "$(NATIVE_BUILD_DIR)"
	@printf 'HOSTCC      %s\n' "$(NATIVE_TRACE_TEST)"
	@set -eu; \
		host_cc="$$(command -v $(HOST_CC))"; \
		host_cc_real="$$(readlink -f "$${host_cc}")"; \
		test_tmp='$(NATIVE_TRACE_TEST).tmp'; \
		report_tmp='$(NATIVE_TRACE_COMPILER_REPORT).tmp'; \
		trap 'rm -f -- "$${test_tmp}" "$${report_tmp}"' EXIT; \
		{ \
			printf 'schema=1\n'; \
			printf 'suite=trace\n'; \
			printf 'compiler_command=%s\n' '$(HOST_CC)'; \
			printf 'compiler_path=%s\n' "$${host_cc}"; \
			printf 'compiler_realpath=%s\n' "$${host_cc_real}"; \
			printf 'compiler_sha256=%s\n' \
				"$$(sha256sum "$${host_cc_real}" | awk '{print $$1}')"; \
			printf 'flags=%s\n' '$(NATIVE_CFLAGS)'; \
			printf '%s\n' 'compiler_version_begin'; \
			"$${host_cc_real}" --version; \
			printf '%s\n' 'compiler_version_end'; \
			sha256sum $(NATIVE_TRACE_TEST_SOURCES) kernel/include/aymos_trace.h; \
		} > "$${report_tmp}"; \
		"$${host_cc_real}" $(NATIVE_CFLAGS) $(NATIVE_TRACE_TEST_SOURCES) \
			-o "$${test_tmp}"; \
		printf 'binary_sha256=%s\n' \
			"$$(sha256sum "$${test_tmp}" | awk '{print $$1}')" \
			>> "$${report_tmp}"; \
		mv -- "$${test_tmp}" '$(NATIVE_TRACE_TEST)'; \
		mv -- "$${report_tmp}" '$(NATIVE_TRACE_COMPILER_REPORT)'; \
		trap - EXIT; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			'$(NATIVE_TRACE_TEST)'; \
		expected="$$(awk -F= '/^binary_sha256=/ {print $$2}' \
			'$(NATIVE_TRACE_COMPILER_REPORT)')"; \
		actual="$$(sha256sum '$(NATIVE_TRACE_TEST)' | awk '{print $$1}')"; \
		test "$${actual}" = "$${expected}"

test-native-dsp: $(NATIVE_DSP_TEST_SOURCES) dsp/include/aymos_fir_q15.h \
	dsp/src/fir_q15_internal.h FORCE
	@mkdir -p "$(NATIVE_BUILD_DIR)"
	@printf 'HOSTCC      %s\n' "$(NATIVE_DSP_TEST)"
	@set -eu; \
		host_cc="$$(command -v $(HOST_CC))"; \
		host_cc_real="$$(readlink -f "$${host_cc}")"; \
		test_tmp='$(NATIVE_DSP_TEST).tmp'; \
		report_tmp='$(NATIVE_DSP_COMPILER_REPORT).tmp'; \
		trap 'rm -f -- "$${test_tmp}" "$${report_tmp}"' EXIT; \
		{ \
			printf 'schema=1\n'; \
			printf 'suite=fir-q15\n'; \
			printf 'compiler_command=%s\n' '$(HOST_CC)'; \
			printf 'compiler_path=%s\n' "$${host_cc}"; \
			printf 'compiler_realpath=%s\n' "$${host_cc_real}"; \
			printf 'compiler_sha256=%s\n' \
				"$$(sha256sum "$${host_cc_real}" | awk '{print $$1}')"; \
			printf 'flags=%s\n' '$(NATIVE_DSP_CFLAGS)'; \
			printf '%s\n' 'compiler_version_begin'; \
			"$${host_cc_real}" --version; \
			printf '%s\n' 'compiler_version_end'; \
			sha256sum $(NATIVE_DSP_TEST_SOURCES) \
				dsp/include/aymos_fir_q15.h dsp/src/fir_q15_internal.h; \
		} > "$${report_tmp}"; \
		"$${host_cc_real}" $(NATIVE_DSP_CFLAGS) \
			$(NATIVE_DSP_TEST_SOURCES) -o "$${test_tmp}"; \
		printf 'binary_sha256=%s\n' \
			"$$(sha256sum "$${test_tmp}" | awk '{print $$1}')" \
			>> "$${report_tmp}"; \
		mv -- "$${test_tmp}" '$(NATIVE_DSP_TEST)'; \
		mv -- "$${report_tmp}" '$(NATIVE_DSP_COMPILER_REPORT)'; \
		trap - EXIT; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			'$(NATIVE_DSP_TEST)'; \
		expected="$$(awk -F= '/^binary_sha256=/ {print $$2}' \
			'$(NATIVE_DSP_COMPILER_REPORT)')"; \
		actual="$$(sha256sum '$(NATIVE_DSP_TEST)' | awk '{print $$1}')"; \
		test "$${actual}" = "$${expected}"

$(DSP_CODEGEN_CONFIG): Makefile tools/setup/dependencies.lock FORCE | check-setup
	@mkdir -p "$(dir $@)"
	@set -eu; \
		rm -f -- '$(DSP_CODEGEN_REPORT)'; \
		config_tmp='$@.tmp'; \
		trap 'rm -f -- "$${config_tmp}"' EXIT; \
		{ \
			printf 'schema=1\n'; \
			printf 'compiler=%s\n' '$(CC)'; \
			printf 'compiler_version=%s\n' "$$($(CC) -dumpfullversion)"; \
			printf 'compiler_sha256=%s\n' \
				"$$(sha256sum '$(CC)' | awk '{print $$1}')"; \
			printf 'flags=%s\n' '$(DSP_ARM_CFLAGS)'; \
			printf 'dependency_lock_sha256=%s\n' \
				"$$(sha256sum tools/setup/dependencies.lock | awk '{print $$1}')"; \
			printf 'cmsis_compiler_sha256=%s\n' \
				"$$(sha256sum '$(CMSIS_CORE_DIR)/cmsis_compiler.h' | \
					awk '{print $$1}')"; \
		} > "$${config_tmp}"; \
		if test -f '$@' && cmp -s "$${config_tmp}" '$@'; then \
			rm -f -- "$${config_tmp}"; \
		else \
			mv -- "$${config_tmp}" '$@'; \
		fi; \
		trap - EXIT

$(DSP_CODEGEN_M4_OBJECT): dsp/src/fir_q15_m4.c \
	dsp/src/fir_q15_internal.h dsp/include/aymos_fir_q15.h | check-setup
$(DSP_CODEGEN_M4_OBJECT): $(DSP_CODEGEN_CONFIG)
	@mkdir -p "$(dir $@)"
	@printf 'CC(dsp-m4)  %s\n' "dsp/src/fir_q15_m4.c"
	@$(CC) $(DSP_ARM_CFLAGS) -c "dsp/src/fir_q15_m4.c" -o "$@"

$(DSP_CODEGEN_COMMON_OBJECT): dsp/src/fir_q15_common.c \
	dsp/src/fir_q15_internal.h dsp/include/aymos_fir_q15.h | check-setup
$(DSP_CODEGEN_COMMON_OBJECT): $(DSP_CODEGEN_CONFIG)
	@mkdir -p "$(dir $@)"
	@printf 'CC(dsp)     %s\n' "dsp/src/fir_q15_common.c"
	@$(CC) $(DSP_ARM_CFLAGS) -c "dsp/src/fir_q15_common.c" -o "$@"

$(DSP_CODEGEN_PACKED_OBJECT): dsp/src/fir_q15_packed_portable.c \
	dsp/src/fir_q15_internal.h dsp/include/aymos_fir_q15.h | check-setup
$(DSP_CODEGEN_PACKED_OBJECT): $(DSP_CODEGEN_CONFIG)
	@mkdir -p "$(dir $@)"
	@printf 'CC(dsp)     %s\n' "dsp/src/fir_q15_packed_portable.c"
	@$(CC) $(DSP_ARM_CFLAGS) -c "dsp/src/fir_q15_packed_portable.c" -o "$@"

$(DSP_CODEGEN_SCALAR_OBJECT): dsp/src/fir_q15_scalar.c \
	dsp/src/fir_q15_internal.h dsp/include/aymos_fir_q15.h | check-setup
$(DSP_CODEGEN_SCALAR_OBJECT): $(DSP_CODEGEN_CONFIG)
	@mkdir -p "$(dir $@)"
	@printf 'CC(dsp-ref) %s\n' "dsp/src/fir_q15_scalar.c"
	@$(CC) $(DSP_ARM_CFLAGS) -c "dsp/src/fir_q15_scalar.c" -o "$@"

check-dsp-codegen:
	@rm -f -- "$(DSP_CODEGEN_REPORT)"
	@$(MAKE) --no-print-directory check-dsp-codegen-internal

check-dsp-codegen-internal: $(DSP_CODEGEN_COMMON_OBJECT) $(DSP_CODEGEN_M4_OBJECT) \
	$(DSP_CODEGEN_PACKED_OBJECT) $(DSP_CODEGEN_SCALAR_OBJECT) \
	tools/check_dsp_codegen.py | check-setup
	@rm -f -- "$(DSP_CODEGEN_REPORT)"
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) tools/check_dsp_codegen.py \
			--objdump "$(OBJDUMP)" \
			--build-config "$(DSP_CODEGEN_CONFIG)" \
			--m4-object "$(DSP_CODEGEN_M4_OBJECT)" \
			--scalar-object "$(DSP_CODEGEN_SCALAR_OBJECT)" \
			--output "$(DSP_CODEGEN_REPORT)"

run: firmware check-renode-platform
	@AYMOS_APP="$(APP)" AYMOS_WORKLOAD_MODE="$(WORKLOAD_MODE)" \
		AYMOS_SIGNAL_IMPL="$(SIGNAL_IMPL)" \
		./tools/renode/run.sh

run-lifecycle:
	@$(MAKE) --no-print-directory APP=lifecycle run

run-edf:
	@$(MAKE) --no-print-directory APP=edf run

run-allocator:
	@$(MAKE) --no-print-directory APP=allocator run

run-trace:
	@$(MAKE) --no-print-directory APP=trace run

dsp:
	@$(MAKE) --no-print-directory APP=signal_lab \
		SIGNAL_IMPL="$(SIGNAL_IMPL)" firmware

signal-lab: dsp

run-dsp:
	@$(MAKE) --no-print-directory APP=signal_lab \
		SIGNAL_IMPL="$(SIGNAL_IMPL)" run

run-signal-lab: run-dsp

test-dsp: check-renode-platform
	@$(MAKE) --no-print-directory APP=signal_lab SIGNAL_IMPL=scalar firmware
	@$(MAKE) --no-print-directory APP=signal_lab SIGNAL_IMPL=m4 firmware
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m tools.aymos_lab.signal_lab test

test-signal-lab: test-dsp

report-dsp: check-setup
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m tools.aymos_lab.performance_report

demo-dsp: check-renode-platform
	@$(MAKE) --no-print-directory test-dsp
	@$(MAKE) --no-print-directory report-dsp

workload:
	@$(MAKE) --no-print-directory APP=deadline_lab \
		WORKLOAD_MODE="$(WORKLOAD_MODE)" firmware

deadline-lab: workload

quickstart:
	@$(MAKE) --no-print-directory setup
	@$(MAKE) --no-print-directory demo

demo:
	@$(MAKE) --no-print-directory APP=deadline_lab \
		WORKLOAD_MODE=normal firmware
	@$(MAKE) --no-print-directory APP=deadline_lab \
		WORKLOAD_MODE=overload firmware
	@env -u PYTHONHOME -u PYTHONPATH \
		PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
		$(PYTHON) -m tools.aymos_lab.deadline_lab demo

test-emulator: firmware check-renode-platform
	@AYMOS_APP="$(APP)" ./tools/renode/test.sh

test-emulator-offline: firmware check-renode-platform
	@AYMOS_APP="$(APP)" ./tools/renode/test.sh --offline

lifecycle:
	@$(MAKE) --no-print-directory APP=lifecycle firmware

test-lifecycle:
	@$(MAKE) --no-print-directory APP=lifecycle test-emulator

edf:
	@$(MAKE) --no-print-directory APP=edf firmware

test-edf:
	@$(MAKE) --no-print-directory APP=edf test-emulator

allocator:
	@$(MAKE) --no-print-directory APP=allocator firmware

test-allocator:
	@$(MAKE) --no-print-directory APP=allocator test-emulator

trace:
	@$(MAKE) --no-print-directory APP=trace firmware

test-trace: trace
	@RENODE_REPEAT="$${RENODE_REPEAT:-1}" AYMOS_APP=trace \
		./tools/renode/test.sh

$(PROJECT_OBJECTS) $(DSP_PROJECT_OBJECTS) $(PROJECT_ASM_OBJECTS) \
	$(VENDOR_C_OBJECTS) $(VENDOR_ASM_OBJECTS): | check-setup

$(PROJECT_OBJECTS): $(OBJ_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	@printf 'CC(project) %s\n' "$<"
	@$(CC) $(COMMON_CPPFLAGS) $(COMMON_CFLAGS) $(PROJECT_WARNINGS) \
		$(PROJECT_CPPFLAGS) \
		$(DEPENDENCY_FLAGS) -c "$<" -o "$@"

$(DSP_PROJECT_OBJECTS): $(OBJ_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	@printf 'CC(dsp-O2)  %s\n' "$<"
	@$(CC) $(COMMON_CPPFLAGS) $(PROJECT_CPPFLAGS) \
		$(DSP_FIRMWARE_CFLAGS) $(PROJECT_WARNINGS) -Wconversion \
		$(DEPENDENCY_FLAGS) -c "$<" -o "$@"

$(VENDOR_C_OBJECTS): $(OBJ_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	@printf 'CC(vendor)  %s\n' "$<"
	@$(CC) $(COMMON_CPPFLAGS) $(COMMON_CFLAGS) $(VENDOR_WARNINGS) \
		$(DEPENDENCY_FLAGS) -c "$<" -o "$@"

$(VENDOR_ASM_OBJECTS): $(OBJ_DIR)/%.o: %.s
	@mkdir -p "$(dir $@)"
	@printf 'AS(vendor)  %s\n' "$<"
	@$(CC) $(COMMON_CPPFLAGS) $(ARCH_FLAGS) -x assembler-with-cpp \
		-MMD -MP -c "$<" -o "$@"

$(PROJECT_ASM_OBJECTS): $(OBJ_DIR)/%.o: %.S
	@mkdir -p "$(dir $@)"
	@printf 'AS(project) %s\n' "$<"
	@$(CC) $(COMMON_CPPFLAGS) $(ARCH_FLAGS) -x assembler-with-cpp \
		-MMD -MP -c "$<" -o "$@"

$(ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p "$(dir $@)"
	@printf 'LD           %s\n' "$@"
	@$(CC) $(LDFLAGS) $(OBJECTS) -o "$@"

$(MAP): $(ELF)
	@test -f "$@"

$(BIN): $(ELF)
	@printf 'OBJCOPY      %s\n' "$@"
	@$(OBJCOPY) -O binary "$<" "$@"

$(SIZE_REPORT): $(ELF)
	@$(SIZE) --format=berkeley "$<" | tee "$@"

FORCE:

$(BUILD_METADATA): FORCE $(ELF) tools/setup/dependencies.lock | check-setup
	@{ \
		printf 'project=%s\n' '$(PROJECT)'; \
		printf 'board=%s\n' '$(BOARD)'; \
		printf 'app=%s\n' '$(APP)'; \
		printf 'git_commit=%s\n' "$$(git rev-parse HEAD)"; \
		printf 'repository_clean=%s\n' "$$(test -z "$$(git status --porcelain)" && printf true || printf false)"; \
		printf 'dependencies_verified=true\n'; \
		printf 'dependencies_clean=true\n'; \
		printf 'compiler=%s\n' "$$($(CC) -dumpfullversion)"; \
		printf 'compiler_path=%s\n' '$(CC)'; \
		printf 'architecture_flags=%s\n' '$(ARCH_FLAGS)'; \
		printf 'float_abi=soft\n'; \
		printf 'dsp_optimization=%s\n' '$(if $(filter signal_lab,$(APP)),-O2,not_applicable)'; \
		printf 'renode=%s\n' '1.16.1'; \
		printf 'python=%s\n' '3.12.13'; \
		printf 'workload_mode=%s\n' '$(if $(filter deadline_lab,$(APP)),$(WORKLOAD_MODE),none)'; \
		printf 'signal_impl=%s\n' '$(if $(filter signal_lab,$(APP)),$(SIGNAL_IMPL),none)'; \
		printf 'trace_schema_version=%s\n' '$(if $(filter trace deadline_lab signal_lab,$(APP)),1,disabled)'; \
		printf 'trace_framing_version=%s\n' '$(if $(filter trace deadline_lab signal_lab,$(APP)),1,disabled)'; \
		printf 'trace_record_size=%s\n' '$(if $(filter trace deadline_lab signal_lab,$(APP)),32,0)'; \
		printf 'trace_footer_size=%s\n' '$(if $(filter trace deadline_lab signal_lab,$(APP)),28,0)'; \
		printf 'trace_ring_records=%s\n' '$(if $(filter trace deadline_lab signal_lab,$(APP)),256,0)'; \
		printf 'stm32cube_f4=%s\n' "$$(git -C .deps/stm32cube_f4_core rev-parse HEAD)"; \
		printf 'cmsis_device_f4=%s\n' "$$(git -C $(CMSIS_DEVICE_DIR) rev-parse HEAD)"; \
		printf 'stm32f4xx_hal=%s\n' "$$(git -C $(HAL_DIR) rev-parse HEAD)"; \
	} > "$@"

validate: $(ELF) $(MAP) $(SIZE_REPORT) $(BUILD_METADATA)
	@CROSS_COMPILE="$(CROSS_COMPILE)" \
		./tools/validate_firmware.sh "$(ELF)" "$(MAP)" "$(BUILD_DIR)"

disassembly: $(ELF)
	@$(OBJDUMP) -d -S "$<" > "$(BUILD_DIR)/$(PROJECT).lst"
	@printf '%s\n' "$(BUILD_DIR)/$(PROJECT).lst"

flash: $(BIN)
	@printf 'AymOS created the NUCLEO-F401RE binary: %s\n' "$(BIN)" >&2
	@printf 'Configure an ST-LINK tool and use it to program this file.\n' >&2
	@exit 2

clean-build:
	@rm -rf -- "$(BUILD_DIR)"

clean-emulator:
	@rm -rf -- "build/renode"

clean: clean-build clean-emulator

help:
	@printf '%s\n' \
		'make quickstart   Install tools and create the scheduling report' \
		'make setup        Install and verify pinned project-local dependencies' \
		'make firmware     Build and validate the F401RE boot firmware (default)' \
		'make test         Run native and host parser/platform tests' \
		'make test-native  Run scheduler, allocator, trace, and FIR C tests' \
		'make test-native-dsp  Run Q15 FIR correctness tests under sanitizers' \
		'make check-dsp-codegen  Check Cortex-M4 FIR instruction selection' \
		'make run          Boot the exact F401RE ELF headlessly and print UART' \
		'make run-lifecycle  Build/run the SVC/PendSV/PSP lifecycle scenario' \
		'make run-edf      Build/run the deterministic two-task EDF scenario' \
		'make run-allocator  Build/run repeated task-owned allocation scenario' \
		'make run-trace    Build/run and decode the structured trace scenario' \
		'make dsp SIGNAL_IMPL=scalar|m4  Build one DSP workload image' \
		'make run-dsp SIGNAL_IMPL=scalar|m4  Run one DSP workload image' \
		'make test-dsp     Run and compare both DSP images once' \
		'make demo-dsp     Run both DSP images and create one report' \
		'make report-dsp   Create a report from existing DSP evidence' \
		'make workload WORKLOAD_MODE=normal|overload  Build one workload mode' \
		'make demo         Run both scheduling modes and create HTML reports' \
		'make test-emulator Run the bounded Renode/Robot UART boot test' \
		'make test-emulator-offline  Repeat the test in a network namespace' \
		'make test-lifecycle  Assert the ARM lifecycle scenario in Renode' \
		'make test-edf     Assert the exact ARM EDF sequence in Renode' \
		'make test-allocator Assert allocator/task-slot reuse in Renode' \
		'make test-trace   Assert one exact ARM trace workload' \
		'make test-host-scheduling  Test the timeline and workload model' \
		'make decode-trace TRACE_INPUT=uart.bin  Strictly decode a saved trace' \
		'make validate     Re-run ELF, map, ABI, and memory validation' \
		'make disassembly  Generate an annotated disassembly' \
		'make clean        Remove firmware and emulator build artifacts' \
		'make flash        Build the binary and print ST-LINK guidance' \
		'' \
		'See docs/BUILDING.md for low-level build selectors.'

-include $(DEPENDENCY_FILES)
