*** Settings ***
Test Timeout                      10 seconds

*** Variables ***
${FIRMWARE_ELF}                   %{AYMOS_FIRMWARE_ELF}
${BOOT_SCRIPT}                    %{AYMOS_BOOT_SCRIPT}
${PLATFORM}                       %{AYMOS_PLATFORM}
${UART_CAPTURE}                   %{AYMOS_UART_CAPTURE}

*** Test Cases ***
F401RE Kernel Executes Complete Lifecycle On PSP
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
    Wait For Line On Uart         LIFECYCLE BEGIN  timeout=5
    Wait For Line On Uart         LIFECYCLE START A ARG=165 PSP=1  timeout=5
    Wait For Line On Uart         LIFECYCLE YIELD A_TO_B  timeout=5
    Wait For Line On Uart         LIFECYCLE START B ARG=90 PSP=1  timeout=5
    Wait For Line On Uart         LIFECYCLE YIELD B_TO_A  timeout=5
    Wait For Line On Uart         LIFECYCLE RESUME A  timeout=5
    Wait For Line On Uart         LIFECYCLE SLEEP A  timeout=5
    Wait For Line On Uart         LIFECYCLE RESUME B  timeout=5
    Wait For Line On Uart         LIFECYCLE PREEMPT B_TO_A  timeout=5
    Wait For Line On Uart         LIFECYCLE RETURN A  timeout=5
    Wait For Line On Uart         LIFECYCLE RECLAIM A  timeout=5
    Wait For Line On Uart         LIFECYCLE CONTINUE B  timeout=5
    Wait For Line On Uart         LIFECYCLE RETURN B  timeout=5
    Wait For Line On Uart         LIFECYCLE RECLAIM B  timeout=5
    Wait For Line On Uart         LIFECYCLE IDLE PSP=1  timeout=5
    Wait For Line On Uart         AYMOS LIFECYCLE PASS  timeout=5

    Execute Command               pause
    ${vtor}=                      Execute Command  sysbus ReadDoubleWord 0xE000ED08
    Should Be Equal As Numbers    ${vtor}  0x08000000
