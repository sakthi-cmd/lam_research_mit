# Lam Research Challenge — CASE 1

ESP32 firmware for continuous water recirculation, sump high-level response and flow logging. Main sketch: `Lam_Case1_Final.ino`.

## Flow path and operation

```text
TK-01 (10 L) → solenoid → P1 → TK-02 sump → P2 → FT-01 → TK-01
```

- Normal operation: P1 at 50% PWM, P2 at 100%; P3 disabled.
- Sump high level: P1 stops while P2 continues.
- Level clear for one second: P1 restarts.
- No overall run-time limit. Operator stop or a flow fault stops both pumps and commands the valve closed.
- Serial commands replace the physical start switch in this version. CASE 2 is not included.

## Hardware configuration

Use a classic ESP32-WROOM-32 development board with Arduino-ESP32 core 2.x or 3.x.

| Function | GPIO |
|---|---|
| P1 RPWM / LPWM / R_EN / L_EN | 33 / 32 / 26 / 13 |
| P2 RPWM / LPWM / R_EN / L_EN | 19 / 18 / 17 / 16 |
| Float / flow sensor / solenoid control | 35 / 14 / 27 |

P2 runs through **LPWM18**. With `P2_RPWM_HARD_GROUNDED=false`, connect GPIO19 to driver RPWM; do not externally ground GPIO19. For driver RPWM wired directly to driver GND, disconnect its GPIO19 lead and set this option to `true`.

GPIO35 needs an external pull-up to **3.3 V**. The default `FLOAT_HIGH=LOW` must correspond to the actual high-water position. GPIO14 must receive a 3.3 V-compatible signal. Motors and valve use their drivers, never GPIO power. Verify P2's actual pumping direction.

## Start and commands

Upload the sketch. Open Serial Monitor at **115200 baud**, select **Newline**, and send one command per line.

| Command | Action |
|---|---|
| `a` | Arm while idle |
| `s` | Start CASE 1 after arming |
| `x` | Stop immediately |
| `r` | Acknowledge a fault after correcting its cause |
| `k VALUE` | Set measured pulses/litre while idle |
| `p` | Toggle CSV / Serial Plotter output; requires calibration |
| `h` | Show help |

Prime both pumps, open the intended path, leave sump overflow space and keep pump inlets supplied. Send `a`, then `s`. Begin with a short supervised test and use `x` to stop.

## Calibration and evidence

Collect a measured volume downstream of FT-01 while counting the corresponding pulses:

```text
K = (final pulse count − initial pulse count) / collected litres
```

Enter `k` followed by your measured value. Do not substitute example or synthetic data. Calibration is stored in RAM only; re-enter it after reset. Without K, flow and volume fields show `nan`.

CSV records time, state, float status, output commands, pulses, Hz, mL/s, L/min and returned litres. Plot mode shows mL/s and L/min. Total returned litres measure recirculated throughput, not remaining tank volume. Switching serial tools may reset the board.

## Limits and safety

- No initial pulses within 20 seconds, or no pulses for 3 seconds after detection, triggers a latched fault. Lost pulses do not prove a tank is empty.
- There is no dedicated low-water protection. Never wait for the timeout if a pump is dry or a tank is overflowing.
- Output commands do not confirm physical motor or valve operation. A removed solenoid cannot provide isolation.
- Disconnect power before rewiring. Do not operate an overheating/resetting ESP32 or an intermittent driver.
- This README describes firmware behavior, not proof of completed calibration or hardware validation. The challenge's physical-switch requirement needs organiser approval if replaced by serial start.
