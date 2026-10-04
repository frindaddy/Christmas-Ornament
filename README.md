# Christmas Ornament

A battery-powered, shake-activated LED ornament built around a Puya PY32F002B microcontroller. The KiCad files contain the schematic and PCB design; the firmware sleeps until the vibration switch detects movement, then alternates two LED groups.

## How It Works

- A CR2032 coin cell powers the board.
- An HX-0805-C2 vibration switch signals movement to PB0.
- The MCU wakes from STOP mode on either edge of the switch signal.
- Two LED groups, driven through N-channel MOSFETs from PB1 and PA7, alternate every 250 ms for about 5 seconds.
- The MCU returns to STOP mode after the pattern finishes.

The firmware targets the PY32F002BL15S7TU in an SOP-8 package. Battery life depends on the assembled circuit, components, and usage; no measured runtime is specified here.

## Repository Layout

| Path | Contents |
| --- | --- |
| [`hardware/Ornament.kicad_sch`](hardware/Ornament.kicad_sch) | KiCad schematic |
| [`hardware/Ornament.kicad_pcb`](hardware/Ornament.kicad_pcb) | KiCad PCB layout |
| [`hardware/Ornament.pretty/`](hardware/Ornament.pretty/) | Project-specific KiCad footprints |
| [`datasheets/`](datasheets/) | PY32F002B datasheet and reference manual |
| [`software/main.c`](software/main.c) | Application firmware |
| [`software/ornament.uvprojx`](software/ornament.uvprojx) | Keil µVision project |
| [`software/PY32F0xx_Drivers/`](software/PY32F0xx_Drivers/) | Used Puya low-level GPIO and EXTI driver sources |

## Firmware Pin Map

| MCU pin | Function |
| --- | --- |
| PB0 | Vibration switch input; external pull resistor is provided by the board |
| PB1 | LED group A MOSFET gate |
| PA7 | LED group B MOSFET gate |
| PA2 | SWCLK programming/debug signal |
| PB6 | SWDIO programming/debug signal |

## Build and Flash

The firmware project is configured for Keil µVision, Arm Compiler 6.24, and the Puya `PY32F0xx_DFP` device pack (version 1.2.8 in the project settings).

1. Install Keil MDK/µVision and the Puya PY32F0xx device family pack.
2. Open [`software/ornament.uvprojx`](software/ornament.uvprojx) in µVision and select the `Target_1` target.
3. Build the project. The configured executable output is `software/Objects/ornament.axf`.
4. Connect a SWD programmer to SWCLK (PA2), SWDIO (PB6), and the board's power/ground, then use µVision's download/debug controls to program the MCU.

## Hardware

Open the schematic and PCB in KiCad. The project includes custom footprints under `hardware/Ornament.pretty/` and local symbol/footprint table configuration files in `hardware/`. Confirm the selected fabrication settings and the PCB revision before ordering boards.