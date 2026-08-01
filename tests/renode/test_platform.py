from __future__ import annotations

from pathlib import Path
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PLATFORM = REPOSITORY_ROOT / "platform" / "renode" / "nucleo_f401re.repl"
RUNTIME_INPUTS = (
    PLATFORM,
    REPOSITORY_ROOT / "platform" / "renode" / "boot.resc",
    REPOSITORY_ROOT / "tests" / "renode" / "boot.robot",
)


class PlatformSourceTests(unittest.TestCase):
    def test_f401_memory_and_cpu_contract_is_explicit(self) -> None:
        source = PLATFORM.read_text(encoding="utf-8")
        for required in (
            "sysbus 0x00000000",
            "sysbus 0x08000000",
            "size: 0x00080000",
            "sysbus 0x20000000",
            "size: 0x00018000",
            'cpuType: "cortex-m4"',
            "systickFrequency: 84000000",
            "usart2: UART.STM32_UART @ sysbus <0x40004400, +0x100>",
            "gpioPortA: GPIOPort.STM32_GPIOPort",
        ):
            self.assertIn(required, source)

    def test_emulator_runtime_inputs_have_no_network_urls(self) -> None:
        for path in RUNTIME_INPUTS:
            source = path.read_text(encoding="utf-8").lower()
            self.assertNotIn("http://", source, path)
            self.assertNotIn("https://", source, path)
            self.assertNotIn("ftp://", source, path)


if __name__ == "__main__":
    unittest.main()
