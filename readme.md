# NTC measurement on BRD2601 (EFR32MG24) + MCP3425

This is a bring-up project, not a finished product. The goal is to **see I2C working**, then **convert one ADC sample into temperature**.

Your analog front end:

```
VDD (3.3 V) -- 50 kΩ --+-- 47 kΩ NTC (B=3960, R25=47 k) -- GND
                       |
                      Vin+  of MCP3425     Vin- = GND
```

At 25 °C, Rntc ≈ 47 kΩ so Vin+ ≈ **1.60 V**. That is inside the MCP3425 2.048 V range.

---

## 0. What you already had

Simplicity Studio 6 + Simplicity SDK 2025.12.1 are installed.

You already generated the official example **I2C SPM Bare Metal** for **BRD2601B**. That example talks to the onboard Si7021. We copied that generated project here and replaced the Si7021 path with MCP3425 + NTC math.

Why copy the example instead of writing a linker script from scratch?

- The SDK is component-based. `i2cspm_baremetal.slcp` lists the chip, board, I2C driver, USB-UART (VCOM), clocks, sleeptimer.
- Studio/SLC then **generates** `autogen/` and `cmake_gcc/`. You do not maintain those by hand unless you know you must.
- Your job is the **application**: `mcp3425.c`, `ntc.c`, and the loop in `i2cspm.c`.

If you want to recreate this yourself in Studio later:

1. File → New → Silicon Labs Project
2. Example: **Platform → i2cspm_baremetal**
3. Board: **BRD2601B** (EFR32MG24)
4. Generate, build, flash. Confirm VCOM prints Si7021 data.
5. Then swap the sensor code like we did here.

---

## 1. Hardware map (BRD2601)

The SLC board file wired **I2C1** to the Thunderboard sensor / EXP I2C pins:

| Signal | MCU pin | Where to clip the scope |
|--------|---------|-------------------------|
| SCL    | **PC04** | EXP I2C_SCL (often pin 16) |
| SDA    | **PC05** | EXP I2C_SDA (often pin 15) |
| GND    | GND     | EXP pin 1 |
| 3.3 V  | VMCU    | EXP pin 2 |

Confirm on the silkscreen of **your** board. Config lives in `config/sl_i2cspm_sensor_config.h`.

Wire the MCP3425:

- VDD → VMCU (3.3 V)
- VSS → GND
- SCL → PC04
- SDA → PC05
- Vin+ → divider tap (between 50 kΩ and NTC)
- Vin− → GND
- Addr pin: A0 part is address **0x68** (8-bit write **0xD0**)

The Thunderboard already has I2C pull-ups on this bus. Extra 4.7 kΩ pull-ups on your PCB are fine; do not leave SDA/SCL floating.

---

## 2. Boot path (what runs before your code)

`main.c` (SDK template, leave it):

1. `sl_main_init()` — clocks, GPIO, I2C, VCOM UART, board enables
2. `app_init()` → `i2cspm_app_init()` in `i2cspm.c`
3. forever: `sl_main_process_action()` then `app_process_action()`

VCOM is 115200 8N1 (`config/sl_iostream_eusart_vcom_config.h`). `printf` goes to the J-Link virtual COM port.

---

## 3. I2C transaction (MCP3425)

Files: `mcp3425.c` / `mcp3425.h`

One sample:

1. **Write** 1 config byte `0x88` = start one-shot, 16-bit, PGA=1
2. Wait ~67 ms (16-bit is 15 samples/s)
3. **Read** 3 bytes: MSB, LSB, config. Bit 7 of config must be 0 (ready)

On the scope you should see:

- Start, address **0xD0**, **ACK**, data `0x88`, ACK, Stop
- After a pause: Start, address **0xD1** (read), ACK, three data bytes

If you see **NACK** on 0xD0: wiring, power, or address. If SCL never moves: GPIO/init. If SCL is stuck low: missing pull-up or a slave holding the bus.

---

## 4. Convert counts → temperature

File: `ntc.c`

MCP3425 does **not** measure 0–3.3 V. PGA=1 full scale is **±2.048 V**.

```
Vadc = code / 32768 * 2.048 V
Rntc = 50 kΩ * Vadc / (3.3 V - Vadc)
T    = 1 / (1/298.15 + ln(Rntc/47000)/3960)   Kelvin
```

Limitation: with a 3.3 V divider, Vin+ exceeds 2.048 V when the NTC is colder than about **13 °C**. Those samples print `SATURATED`. Warm the thermistor with your fingers; the voltage should drop and T should rise.

Sanity check at 25 °C:

- Rntc ≈ 47 kΩ
- Vadc ≈ 1.60 V
- code ≈ 25500

---

## 5. Build, flash, debug

### In Cursor / VS Code (this repo is set up for it)

Install these extensions when prompted: **C/C++**, **CMake Tools**, **Cortex-Debug**, **Serial Monitor**.

1. Open the folder `Documents\NTC_meas\NTC_meas_code` (not the parent `NTC_meas`).
2. Plug in the BRD2601 USB.
3. **Ctrl+Shift+B** builds. Terminal **Run Task → Flash firmware** programs the chip via Commander.
4. **F5** (configuration *Debug BRD2601 (J-Link)*) builds, flashes, and stops at `main`.
5. Serial Monitor: pick the J-Link COM port, **115200** 8N1. You should see a line every second.

You do not need Simplicity Studio open for this. Studio is still useful if you change SDK components in the `.slcp` file.

### In Simplicity Studio 6 (optional)

1. File → Open workspace / add this folder: `Documents\NTC_meas\NTC_meas_code`
2. Open `i2cspm_baremetal.slcp` if Studio asks to generate — skip generate unless you changed components
3. Build
4. Plug BRD2601 USB, select the J-Link, Debug
5. Open the serial terminal on the J-Link COM port, 115200

### Command line (what this repo also supports)

CMake + Ninja + ARM GCC from SLT. From a PowerShell that can see `slt`:

```powershell
cd C:\Users\victo\Documents\NTC_meas\NTC_meas_code\cmake_gcc
cmake --preset project
cmake --build --preset default_config
```

Hex/bin land in `cmake_gcc/build/base/`.

Flash with Simplicity Commander if you want:

```powershell
commander flash cmake_gcc/build/base/i2cspm_baremetal.hex --device EFR32MG24BxxxF1536
```

Put a breakpoint on `mcp3425_read_code` and step. Watch `code`, then `sample.temp_c_x100`.

---

## 6. Best first experiment

1. Unplug Vin+ and tie it to GND. You should get code ≈ 0 and an invalid sample.
2. Tie Vin+ to a known ~1.6 V (or the real divider at room temp). Code should be ~25k.
3. Heat the NTC: Vadc falls, R falls, T rises.

Keep the scope on SCL/SDA until the first ACK. Then trust the UART numbers.

---

## Files that matter

| File | Why |
|------|-----|
| `i2cspm_baremetal.slcp` | Board, chip, I2C, VCOM components |
| `config/sl_i2cspm_sensor_config.h` | I2C1 on PC04/PC05, 100 kHz |
| `main.c` / `app.c` | SDK boot + calls into your app |
| `i2cspm.c` | 1 Hz loop, prints results |
| `mcp3425.c` | I2C read of the ADC |
| `ntc.c` | Divider + beta conversion |
| `autogen/` | Generated init; do not edit unless you know SLC |
