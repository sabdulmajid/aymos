# Fixed-point DSP workload

This workload uses one Q15 finite impulse response (FIR) filter. The
implementation does not allocate memory. Native tests prove that the scalar
and portable paired functions produce the same integer results. The complete
test runs the scalar and M4 functions in separate NUCLEO-F401RE images.

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

## Run the scheduled workload

Run both firmware configurations once:

```sh
make test-dsp
```

The command builds one scalar image and one M4 image. Each image uses the same
SVC, PendSV, PSP, SysTick, and EDF kernel path. The workload has a sampler, a
processor, a verifier, and the idle task.

The sampler generates four frames. Each frame has 128 signed Q15 samples. It
uses the fixed LFSR seed `0x1A2B3C4D`. The processor applies a 16-tap FIR and
produces 113 valid outputs per frame. The verifier checks each firmware result.
It also checks these aggregate CRC-32 values:

- input: `0x0CAF72FD`;
- output: `0xAFC277C1`.

The tasks release at fixed ticks. The host validator requires the complete
103-record trace, including every selection and candidate record. It rejects a
different event order, deadline, task lifecycle, or selection reason.

The run also records a synchronous, compressed Renode `PCAndOpcode` execution
trace. The M4 image must enter `aymos_fir_q15_m4()` four times. It must execute
3,616 `smlald` instructions and 452 `ssat` instructions at the exact opcode
addresses from that ELF. The scalar image must enter only the scalar FIR and
must not contain the M4 FIR symbol. The scalar source uses a portable
single-lane loop. At `-O2`, Arm GCC selects `smlalbb` for its single-lane MAC.
The test records exactly 7,232 executed `smlalbb` instructions. The M4 source
explicitly packs two lanes for each `smlald`.

The command writes deterministic evidence under:

```text
build/signal-lab/evidence/
  summary.json
  scalar/
  m4/
```

Each implementation directory contains the exact ELF, map, build metadata,
UART data, structured trace, result JSON, execution trace, emulator log, and
command metadata. `summary.json` records the artifact hashes and compares the
two firmware results. The collector snapshots each ELF, map, and build metadata
file before execution. It binds the run to that ELF hash and rejects a live
build artifact or Git commit change during the run. A lock prevents two
evidence runs from replacing each other. A marked staging directory replaces
the prior evidence only after both runs pass. The replacement first renames the
old evidence to a marked backup on the same file system. A failed install
restores that backup. A later run restores or removes a backup left by a sudden
host stop.

Build or run one configuration when you need a shorter iteration:

```sh
make dsp SIGNAL_IMPL=scalar
make run-dsp SIGNAL_IMPL=m4
```

The complete comparison gate is `make test-dsp`.

## Create the comparison report

Run both images once and publish one report:

```sh
make demo-dsp
```

Create another report from the existing validated evidence:

```sh
make report-dsp
```

The report-only command does not run Renode. It reads
`build/signal-lab/evidence/`. It requires the fixed result and the exact common
103-record trace. It verifies every artifact byte count and SHA-256 before it
uses the artifact. It rejects a missing, unknown, changed, oversized, or
nonregular input.

The report command creates:

```text
runs/<UTC-id>-<commit>-signal/
  metadata.json
  workload.json
  summary.json
comparison.html
scalar/
m4/
```

Each implementation directory contains the exact 14-file DSP evidence
set. The top metadata hashes each copied and generated report artifact except
itself and the control marker. A marked stage becomes visible through one
same-file-system rename. The tool keeps eight complete marked DSP reports. It
does not remove scheduling demonstration runs, active stages, or unmarked
paths.

The report path supports Linux. It anchors evidence and report directories with
open directory descriptors. It rejects symbolic-link leaves. Retention moves
an owned report to a private name, rechecks its marker and inode, and removes
entries relative to the held descriptor. A concurrent path replacement does
not redirect a read or removal outside the owned directory. Retention checks
the Linux mount ID and device of each directory before it removes any entry.
It refuses ordinary and bind-mounted child trees. It restores the original
report name when safe, or leaves the report under its private quarantine name.

The report shows these executed instruction records:

- scalar: 7,232 single-lane `smlalbb` operations and 42,140 total FIR-body
  instruction records;
- M4: 3,616 packed `smlald` operations, which cover 7,232 products, 452
  `ssat` operations, and 60,252 total FIR-body instruction records.

The packed multiply-accumulate count is half the single-lane count. The M4
function has more total FIR-body records in this first path. This comparison
does not establish whole-function efficiency.

The report also shows the common scheduled task-state timeline. The bars show
the task state that the scheduler selected. They include the task wait at the
tick boundary. They do not show CPU-active time.

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

The DSP workload changes no kernel policy and no trace schema. It uses deterministic
work and checks functional results. The execution trace proves that the M4 DSP
instructions ran in the board-targeted image. It does not prove hardware
timing, interrupt latency, worst-case execution time, or a speedup.

The scheduler trace reports one accounted RUNNING tick for each job. Each task
uses `WFI` after it completes its work and waits for that tick boundary. The
reported RUNNING interval therefore includes wait time. It is not CPU-active
time and it is not a performance measurement.

The README includes a small representative SVG and JSON document from the
validated source commit `d36d3aa15b59d46b7da4a9d8fbea5ab781468cf9`.
The source boundary is the completed and hashed PR 2 evidence. Physical-board
cycle measurement stays in a later milestone.
