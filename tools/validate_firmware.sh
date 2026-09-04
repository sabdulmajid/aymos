#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

die() {
    printf 'validate: error: %s\n' "$*" >&2
    exit 1
}

[[ $# -eq 3 ]] || die "usage: validate_firmware.sh ELF MAP OUTPUT_DIR"
: "${CROSS_COMPILE:?CROSS_COMPILE must name the project-local tool prefix}"

readonly elf="$1"
readonly map="$2"
readonly output_dir="$3"
readonly readelf="${CROSS_COMPILE}readelf"
readonly objdump="${CROSS_COMPILE}objdump"
readonly objcopy="${CROSS_COMPILE}objcopy"
readonly nm="${CROSS_COMPILE}nm"
readonly vector_image="${output_dir}/vector-table.bin"

[[ -f "${elf}" ]] || die "ELF not found: ${elf}"
[[ -f "${map}" ]] || die "map not found: ${map}"

file "${elf}" > "${output_dir}/file.txt"
"${readelf}" -h "${elf}" > "${output_dir}/readelf-header.txt"
"${readelf}" -A "${elf}" > "${output_dir}/readelf-attributes.txt"
"${readelf}" -W -S "${elf}" > "${output_dir}/readelf-sections.txt"
"${readelf}" -W -l "${elf}" > "${output_dir}/readelf-program-headers.txt"
"${nm}" -n --defined-only "${elf}" > "${output_dir}/symbols.txt"
"${objdump}" -h "${elf}" > "${output_dir}/objdump-sections.txt"
"${objdump}" -d --disassemble=Reset_Handler "${elf}" > \
    "${output_dir}/reset-handler-disassembly.txt"
"${objdump}" -d --disassemble=SystemInit "${elf}" > \
    "${output_dir}/system-init-disassembly.txt"
"${objcopy}" --dump-section ".isr_vector=${vector_image}" "${elf}" /dev/null

grep -q 'ELF 32-bit LSB executable, ARM' "${output_dir}/file.txt" ||
    die "file(1) does not identify a 32-bit little-endian ARM executable"
grep -Eq 'Machine:[[:space:]]+ARM' "${output_dir}/readelf-header.txt" ||
    die "ELF machine is not ARM"
grep -Eq 'Flags:.*Version5 EABI.*soft-float ABI' "${output_dir}/readelf-header.txt" ||
    die "ELF is not EABI5 soft-float"
grep -Eq 'Tag_CPU_arch:[[:space:]]+v7E-M' "${output_dir}/readelf-attributes.txt" ||
    die "ELF attributes do not target ARMv7E-M"
if grep -q 'Tag_ABI_VFP_args' "${output_dir}/readelf-attributes.txt"; then
    die "hard-float procedure-call attributes are present"
fi

symbol_address() {
    local name="$1"
    local address

    address="$(awk -v symbol="${name}" '$3 == symbol { print $1; exit }' \
        "${output_dir}/symbols.txt")"
    [[ -n "${address}" ]] || die "required symbol missing: ${name}"
    printf '%s\n' "${address}"
}

hex_value() {
    local value="${1#0x}"
    printf '%u\n' "$((16#${value}))"
}

readonly flash_start=$((0x08000000))
readonly flash_end=$((0x08080000))
readonly ram_start=$((0x20000000))
readonly ram_end=$((0x20018000))

vector_start="$(hex_value "$(symbol_address __vector_table_start__)")"
vector_end="$(hex_value "$(symbol_address __vector_table_end__)")"
reset_handler="$(hex_value "$(symbol_address Reset_Handler)")"
heap_start="$(hex_value "$(symbol_address __aymos_heap_start__)")"
heap_end="$(hex_value "$(symbol_address __aymos_heap_end__)")"
newlib_start="$(hex_value "$(symbol_address __newlib_heap_start__)")"
newlib_end="$(hex_value "$(symbol_address __newlib_heap_end__)")"
stack_limit="$(hex_value "$(symbol_address __msp_stack_limit__)")"
stack_top="$(hex_value "$(symbol_address __msp_stack_top__)")"
estack="$(hex_value "$(symbol_address _estack)")"
entry_address="$(awk -F: '/Entry point address:/ {gsub(/[[:space:]]/, "", $2); print $2}' \
    "${output_dir}/readelf-header.txt")"
[[ -n "${entry_address}" ]] || die "could not parse ELF entry address"
entry="$(hex_value "${entry_address}")"

((vector_start == flash_start)) || die "vector table is not at flash origin"
((vector_start % 0x200 == 0)) || die "vector table is not 0x200 aligned"
((vector_end > vector_start && vector_end <= vector_start + 0x200)) ||
    die "vector table does not fit its 0x200 alignment slot"
((reset_handler >= flash_start && reset_handler < flash_end)) ||
    die "Reset_Handler lies outside flash"
((entry == (reset_handler | 1))) ||
    die "ELF entry is not the Thumb Reset_Handler address"
((heap_start >= ram_start && heap_start <= heap_end)) ||
    die "AymOS heap start is outside RAM"
((heap_end == stack_limit)) || die "heap end and MSP stack limit disagree"
((heap_start % 8 == 0 && heap_end % 8 == 0)) ||
    die "AymOS heap boundaries are not eight-byte aligned"
((newlib_start == newlib_end)) || die "newlib heap is not empty"
((newlib_start == heap_start)) || die "newlib and AymOS linker contract disagrees"
((stack_top == ram_end && estack == ram_end)) || die "MSP top is not RAM end"
((stack_limit < stack_top)) || die "MSP reservation is empty"

[[ "$(stat -c '%s' "${vector_image}")" == "$((vector_end - vector_start))" ]] ||
    die "dumped vector-table size does not match linker symbols"
read -r vector_msp vector_reset < <(od -An -N8 -t x4 "${vector_image}")
[[ -n "${vector_msp:-}" && -n "${vector_reset:-}" ]] ||
    die "could not read initial MSP/reset vector words"
((16#${vector_msp} == stack_top)) ||
    die "initial vector MSP does not equal the linked MSP top"
((16#${vector_reset} == (reset_handler | 1))) ||
    die "reset vector does not contain the Thumb Reset_Handler address"

mapfile -t system_init_calls < <(
    grep -nE 'bl[.a-z[:space:]]+.*<SystemInit>' \
        "${output_dir}/reset-handler-disassembly.txt" || true
)
mapfile -t main_calls < <(
    grep -nE 'bl[.a-z[:space:]]+.*<main>' \
        "${output_dir}/reset-handler-disassembly.txt" || true
)
[[ ${#system_init_calls[@]} -eq 1 ]] ||
    die "Reset_Handler must contain exactly one parseable SystemInit call"
[[ ${#main_calls[@]} -eq 1 ]] ||
    die "Reset_Handler must contain exactly one parseable main call"
system_init_line="${system_init_calls[0]%%:*}"
main_line="${main_calls[0]%%:*}"
[[ "${system_init_line}" =~ ^[0-9]+$ && "${main_line}" =~ ^[0-9]+$ ]] ||
    die "could not parse Reset_Handler call line numbers"
((system_init_line < main_line)) ||
    die "Reset_Handler does not call SystemInit before main"

load_count=0
flash_origin_load=false
while read -r offset_hex vma_hex physical_hex file_size_hex memory_size_hex; do
    load_count=$((load_count + 1))
    vma="$(hex_value "${vma_hex}")"
    physical="$(hex_value "${physical_hex}")"
    file_size="$(hex_value "${file_size_hex}")"
    memory_size="$(hex_value "${memory_size_hex}")"
    vma_end=$((vma + memory_size))
    physical_end=$((physical + file_size))

    ((memory_size >= file_size)) ||
        die "PT_LOAD ${offset_hex} has MemSiz smaller than FileSiz"
    ((vma_end >= vma && physical_end >= physical)) ||
        die "PT_LOAD ${offset_hex} address arithmetic overflowed"

    if ! ((
        (vma >= flash_start && vma_end <= flash_end) ||
        (vma >= ram_start && vma_end <= ram_end)
    )); then
        die "PT_LOAD ${offset_hex} VMA range lies outside F401RE memory"
    fi

    if ((file_size > 0)) && ! ((
        physical >= flash_start && physical_end <= flash_end
    )); then
        die "PT_LOAD ${offset_hex} nonempty file payload does not originate in flash"
    fi

    if ((vma == flash_start && physical == flash_start && file_size > 0)); then
        flash_origin_load=true
    fi
done < <(
    awk '$1 == "LOAD" { print $2, $3, $4, $5, $6 }' \
        "${output_dir}/readelf-program-headers.txt"
)
((load_count > 0)) || die "ELF has no PT_LOAD program headers"
[[ "${flash_origin_load}" == true ]] ||
    die "ELF has no nonempty PT_LOAD whose VMA and LMA start at flash origin"

while read -r section size_hex vma_hex; do
    section_size="$(hex_value "${size_hex}")"
    section_start="$(hex_value "${vma_hex}")"
    section_end=$((section_start + section_size))

    if ((section_size == 0)); then
        continue
    fi
    if ((section_start >= flash_start && section_end <= flash_end)); then
        continue
    fi
    if ((section_start >= ram_start && section_end <= ram_end)); then
        continue
    fi
    die "allocated section ${section} at ${vma_hex}+${size_hex} is outside F401RE memory"
done < <(
    awk '
        $1 ~ /^[0-9]+$/ {
            name = $2; size = $3; vma = $4;
            getline flags;
            if (flags ~ /ALLOC/) print name, size, vma;
        }
    ' "${output_dir}/objdump-sections.txt"
)

grep -q '__aymos_heap_start__' "${map}" || die "map omits AymOS heap symbols"
grep -q '__newlib_heap_end__' "${map}" || die "map omits newlib heap symbols"
grep -q '__msp_stack_top__' "${map}" || die "map omits MSP stack symbols"

printf 'validate: F401RE ELF, soft-float ABI, vector table, and memory map are valid\n'
