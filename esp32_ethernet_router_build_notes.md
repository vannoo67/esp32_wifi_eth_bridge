# Building esp32_ethernet_router on a plain ESP32 + W5500 module

Notes from getting [martin-ger/esp32_ethernet_router](https://github.com/martin-ger/esp32_ethernet_router)
running on a generic ESP32-WROOM dev kit with an external W5500 SPI Ethernet module
(the repo's own build variants target either WT32-ETH01 or ESP32-C3 + W5500 — neither
matches this hardware combo exactly, hence the extra steps below).

## Hardware

- Generic ESP32-WROOM dev kit
- W5500 SPI Ethernet breakout module (with its own onboard AMS1117 3.3V regulator)
- USB cable + **direct connection to a PC or wall charger** — see [Power supply](#power-supply-issues) below

### Wiring

| W5500 Pin | ESP32 GPIO | Function        |
|-----------|-----------|------------------|
| GND       | GND        | Ground          |
| 5V / VCC  | VIN        | Power (unregulated 5V, fed through the module's own AMS1117) |
| MISO      | GPIO 13    | SPI Data In     |
| MOSI      | GPIO 14    | SPI Data Out    |
| SCLK      | GPIO 33    | SPI Clock       |
| SCS (CS)  | GPIO 26    | SPI Chip Select |
| INT       | GPIO 25    | Interrupt       |
| RST       | GPIO 27    | Hardware Reset  |

**Notes on pin choice:**
- Avoid ESP32 strapping pins for these signals: GPIO0, GPIO2, GPIO12 (MTDI — can force
  the wrong flash voltage at boot if pulled high), and GPIO15.
- GPIO14 (MTMS/JTAG) is fine to use as long as you're not also using a JTAG debugger.
- **Double-check every wire against this table before powering on.** MISO/MOSI swaps in
  particular don't cause a build error — they just make SPI communication silently fail
  (see [Troubleshooting](#troubleshooting)).

## 1. Install ESP-IDF (standalone — not via PlatformIO)

This project is a native ESP-IDF project (root `CMakeLists.txt`, `idf.py`-driven build,
multiple build variants). Trying to force it into PlatformIO's project model is more
trouble than it's worth. Use the official ESP-IDF tooling instead:

1. Download and run the [ESP-IDF Windows installer](https://dl.espressif.com/dl/esp-idf/)
   and install the latest **v5.x** release (this project was built and tested against
   **v5.5.5**; v6.x changes some ethernet driver internals — see step 5).
2. This installs an **ESP-IDF Command Prompt** / **ESP-IDF PowerShell** shortcut in the
   Start menu. Use that terminal for everything below — it sets up `IDF_PATH` and the
   toolchain automatically.

## 2. Clone the repo

```
git clone https://github.com/martin-ger/esp32_ethernet_router.git
cd esp32_ethernet_router
```

## 3. Set the target chip

```
idf.py set-target esp32
```

(The repo's docs/scripts assume WT32-ETH01 or ESP32-C3; a plain ESP32 needs this
explicitly.)

## 4. Configure the W5500 driver and pins (project-level menuconfig)

```
idf.py menuconfig
```

Search (`/`) for **"Ethernet downlink driver"**, open it, and select
**"SPI Ethernet W5500"** (default is the internal EMAC/LAN8720 driver, which is wrong
for this setup). This reveals SPI-specific fields — enter your wiring:

```
SPI host number        : 1 (SPI2_HOST) — default is fine
W5500 MISO GPIO        : 13
W5500 MOSI GPIO        : 14
W5500 SCLK GPIO        : 33
W5500 CS (SCS) GPIO    : 26
W5500 INT GPIO         : 25
W5500 RST GPIO         : 27
W5500 SPI clock speed  : 10 MHz — conservative default, raise later once stable
```

**Do not exit menuconfig yet — step 5 is in the same session.**

## 5. Enable ESP-IDF's own W5500 driver support (separate from step 4!)

This is the easy-to-miss step. The project's own "Ethernet Downlink" menu (step 4)
only controls *this repo's* internal driver selection logic. It does **not** enable
ESP-IDF's own built-in W5500 support code — that's a separate Kconfig setting one
level up, in ESP-IDF's core `esp_eth` component.

Still inside `idf.py menuconfig`:

1. Search (`/`) for `ETH_SPI_ETHERNET_W5500`, or navigate to
   **Component config → Ethernet**.
2. Make sure these are enabled:
   - **Support SPI Ethernet Module(s)** (`CONFIG_ETH_USE_SPI_ETHERNET`)
   - Under the SPI Ethernet chip choice, select **"Use W5500 (MAC RAW)"**
     (`CONFIG_ETH_SPI_ETHERNET_W5500`)

> On ESP-IDF v5.x, W5500 support (including the `eth_w5500_config_t` type) is compiled
> directly into the core `esp_eth` component behind this flag — it's **not** a separate
> managed/registry component. (This changed in ESP-IDF v6.0, where W5500 support moved
> to the `espressif/w5500` Component Registry package — don't add that as a dependency
> on v5.x, it will fail with a version-solving error since that package requires
> `idf >= 6.0`.)

Now exit (`Esc`, `Esc`) and save.

## 6. Build

```
idf.py fullclean
idf.py build
```

Use `fullclean` (not just `clean`) after toggling Kconfig options like the one above —
plain `clean` can leave stale CMake/component-manager cache behind and produce
confusing partial-failure results.

## 7. Flash and monitor

Find your board's COM port (Windows: Device Manager → Ports (COM & LPT)), then:

```
idf.py -p COM3 flash monitor
```

(replace `COM3` with your actual port). This flashes and opens the serial console at
115200 baud (ESP-IDF's default — no manual baud config needed here, unlike some
Arduino/PlatformIO setups).

To reopen the monitor later without reflashing:

```
idf.py -p COM3 monitor
```

Exit with `Ctrl+]`.

## 8. First boot — configure WiFi

At the serial console:

```
set_sta <your-ssid> <your-password>
```

Reboot, and the router should come up bridging WiFi (uplink) to the wired Ethernet
port (downlink via W5500), serving DHCP at `192.168.4.1` by default. Check
`w5500 status` to confirm the SPI link to the chip is healthy.

## Power supply issues

The W5500 module can draw up to ~250mA, on top of the ESP32's own draw (higher during
WiFi TX/calibration — the WiFi radio calibration step at boot is a particularly sharp
current spike). Combined, this can exceed what a USB hub port can supply cleanly, even
if the hub's rated total looks sufficient.

**Symptom seen:** `E BOD: Brownout detector was triggered` right as
`phy_init` / WiFi calibration starts.

**Fix:** connect the board directly to a PC USB port or a wall charger — **not** a hub.
If that's not possible, options in rough order of effectiveness:
- Use a shorter, thicker USB cable (thin/long cables have real resistive drop under
  current spikes)
- Add a bulk capacitor (470–1000µF) across 5V/GND near the ESP32 and W5500
- Power the W5500 from a separate 5V source, sharing only GND with the ESP32

## Troubleshooting

### `error: unknown type name 'eth_w5500_config_t'`

Means the internal ESP-IDF W5500 driver code isn't compiled in — you're missing step 5
above (`CONFIG_ETH_USE_SPI_ETHERNET` + `CONFIG_ETH_SPI_ETHERNET_W5500`). This is **not**
a missing managed component on IDF v5.x — don't add `espressif/w5500` to
`idf_component.yml`, it requires IDF ≥6.0 and will fail to resolve.

### Repeated `W5500 register reset detected (SOCK_MR=0x00) — re-initialising`, followed by a task watchdog crash in `w5500_tsk`

The driver keeps finding the chip in its power-on-reset state — either the chip is
actually resetting repeatedly, or SPI communication is bad enough that reads come back
as garbage. Check, in rough order of likelihood:

1. **Wiring** — re-trace every wire against your pin table. A MISO/MOSI swap is a very
   common mistake and won't show up as a build error, only as silent SPI failure.
2. **RST line** — must be driven cleanly high (not floating) to bring the chip out of
   reset. Check the connection is solid.
3. **Power supply** — see [Power supply issues](#power-supply-issues) above; an
   unstable 5V rail under load looks identical to this symptom.
4. **SPI clock** — try lowering it further (e.g. 4–5 MHz) as a diagnostic step; if that
   fixes it, the real issue is signal integrity (long wires / breadboard capacitance)
   rather than a hard fault.

### Garbled/gibberish serial monitor output

Baud rate mismatch — ESP-IDF's console defaults to 115200. `idf.py monitor` gets this
right automatically; this is more of a gotcha in plain PlatformIO/Arduino setups where
the monitor speed can default to something else (e.g. 9600).
