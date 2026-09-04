# Cortex-M4 performance lab

The performance lab starts with one small DSP operation. It uses a Q15
finite impulse response (FIR) filter. The implementation does not allocate
memory. Native tests prove that the scalar and portable paired functions
produce the same integer results. This change does not execute the M4 function.

## Run the checks

Install the pinned project tools once:

```sh
make setup
```

Run the native correctness test:

```sh
make test-native-dsp
```

The test uses strict warnings, AddressSanitizer, and UndefinedBehaviorSanitizer.
It compares the scalar loop and the portable paired loop against an independent
reference. The fixed test corpus includes all tap counts from 1 through 64.

Check the Cortex-M4 object code:

```sh
make check-dsp-codegen
```

The command writes `build/dsp-codegen/codegen-report.json`. The check examines
only the exact FIR function bounds. It requires `smlald` and `ssat` in the M4
function. It rejects these instructions in the scalar function. It also rejects
VFP instructions and a `memcpy` reference in either checked function.

This is static instruction evidence. It is not a hardware timing measurement.
It does not prove execution time, interrupt latency, or a real-time limit.

## FIR contract

Include `dsp/include/aymos_fir_q15.h`.

- The input and coefficients use signed Q15 values.
- The filter accepts 1 through 64 taps.
- Coefficient 0 applies to the oldest sample in each window.
- The output contains only complete windows. Its count is
  `sample_count - tap_count + 1`.
- Each output uses a signed 64-bit Q30 accumulator.
- Division by 32768 converts Q30 to Q15. C signed division truncates toward
  zero.
- The result saturates to the signed 16-bit range.
- The output must not overlap the samples or coefficients.
- The function validates all arguments before it writes an output value.

Use `aymos_fir_q15_scalar()` for the portable reference. Use
`aymos_fir_q15_m4()` only in an Arm DSP build. This implementation packs each
pair with integer shifts and uses the pinned CMSIS Core `__SMLALD` intrinsic.
It uses `__SSAT` for each output. It also processes an odd final tap.

`aymos_fir_q15_packed_portable()` has the same paired-loop order on the host.
It supports fast equivalence tests. It does not execute Arm instructions and
is not evidence of Cortex-M4 execution.

## Current limits

This change does not add the FIR code to a firmware application. It does not
change the kernel, scheduler, trace schema, or Deadline Lab. It provides a
reviewable numerical base and checks the intended M4 instruction selection.
The M4 result-equivalence gate belongs to the board-targeted firmware in the
next change.

The next change will run the scalar and M4 filters in real board-targeted
firmware with deterministic input and result checks. The third campaign change
will generate a bounded scalar-versus-DSP comparison report and representative
README artifacts. Physical-board cycle measurement stays in a later milestone.
