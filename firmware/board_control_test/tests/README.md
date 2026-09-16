# Host driver checks

Run `sh firmware/board_control_test/tests/run.sh` with a C++17 compiler.
The tests compile the actual `BoardControl.cpp` and `ChargerHold.cpp` against small
Arduino/Wire mocks.
They exercise unavailable expanders during retained-state recovery, a CE pin stuck
high, retained polarity inversion, the corrected schematic input map, J8's buck
dependency and timeout, and CE-before-boost shutdown ordering.
Thermistor checks cover both selections, preserving the other outputs, CE-high
refusal, the 100 ms CE-low delay, the post-switch 100 ms override / 500 ms real-NTC
settling waits before a subsequent CE command, direction/latch/physical-pin faults,
and restoring the real NTC during shutdown or initialization with a retained bypass
state.

Measurement-hold checks cover continuous regulation qualification, twelve minutes
of operation without a duration limit, manual stop, latched overvoltage shutdown,
source/PG/STAT loss, expander read failure with incomplete-cleanup reporting, the
open physical thermistor while fixed TS is selected, and startup undervoltage
timeout. ADC readings are configurable; the idealized fixture asserts charger
STAT when CE and the temperature bypass are both enabled.

The mock models TCA9535 paired registers, output direction, polarity and physical
output readback. Time advances without sleeping. PG behavior is an idealized
fixture response; these tests establish software decisions and sequencing, not
electrical timing, voltage regulation or real fault protection.

Additional diagnostics checks:

- `sh firmware/board_control_test/tests/run-expander.sh`: seven cases for separate
  GPIO interrupt capture, short pulses, read acknowledgment, read failure,
  concurrent events and scoped-handler cleanup.
- `sh firmware/board_control_test/tests/pd_run.sh`: read-only little-endian HPI
  transport, decoding, failed-read isolation, malformed responses, IRQ consistency,
  and the actual zero-current RDO reported separately as an unvalidated power request.
