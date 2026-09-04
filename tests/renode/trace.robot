*** Settings ***
Test Timeout                      10 seconds

*** Variables ***
${FIRMWARE_ELF}                   %{AYMOS_FIRMWARE_ELF}
${BOOT_SCRIPT}                    %{AYMOS_BOOT_SCRIPT}
${PLATFORM}                       %{AYMOS_PLATFORM}
${UART_CAPTURE}                   %{AYMOS_UART_CAPTURE}

*** Test Cases ***
F401RE Emits Complete Structured Kernel Trace
    Execute Command               $bin=@${FIRMWARE_ELF}
    Execute Command               $platform=@${PLATFORM}
    Execute Command               include @${BOOT_SCRIPT}

    ${alias_vector}=              Execute Command  sysbus ReadDoubleWord 0x00000000
    ${flash_vector}=              Execute Command  sysbus ReadDoubleWord 0x08000000
    Should Be Equal As Numbers    ${alias_vector}  0x20018000
    Should Be Equal As Numbers    ${flash_vector}  0x20018000

    Execute Command               sysbus.usart2 CreateFileBackend @${UART_CAPTURE}
    Create Terminal Tester        sysbus.usart2  timeout=5
    Start Emulation

    Wait For Line On Uart         AYMOS READY  timeout=5
    Wait For Line On Uart         TRACE BEGIN  timeout=5
    Wait For Line On Uart         AYMOS TRACE DONE  timeout=5

    Execute Command               pause
    ${vtor}=                      Execute Command  sysbus ReadDoubleWord 0xE000ED08
    Should Be Equal As Numbers    ${vtor}  0x08000000
