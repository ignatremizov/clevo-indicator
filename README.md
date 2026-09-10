# Clevo Laptop Fan Speed Utility

Control CPU and GPU fans on supported Clevo laptops.

## Screenshots

Default single-indicator UI:

![Single-indicator UI](assets/sni-single-indicator-ui.png)

Legacy dual-AppIndicator UI:

![Legacy dual-AppIndicator UI](assets/legacy-dual-appindicator-ui.png)

## Features

Use the project to:

- read CPU/GPU temperature, duty, and RPM values
- set CPU and GPU fan duty manually from the CLI
- run automatic fan control based on the hottest component
- open a GNOME panel indicator with a dual-column CPU/GPU control popup

Build on top of:

- https://github.com/davidrohr/clevo-indicator
- https://github.com/SkyLandTW/clevo-indicator

## Hardware Compatibility

This fork targets the Clevo X170KM-G / Xotic XMG170KM used for development. Its EC GPU temperature register is **`0xCD`**, not `0x0A`. Both the indicator's compact EC sample and the CLI register definition use `0xCD`. The indicator prefers a valid, fresh NVIDIA temperature reading and falls back to the EC GPU temperature when that reading is unavailable.

Register mappings differ between models and forks: for example, [gateslu's `v1.0` implementation](https://github.com/gateslu/clevo-indicator/blob/91ec5041eebdd97b2507c8f9c99ef631b68d947d/src/clevo-indicator.cpp#L24) uses **`0x0A`**. Neither address is a universal Clevo mapping. Verify the target model's temperature registers and fan-control protocol before using this fork on another laptop or importing hardware-specific changes; these mappings are currently hardcoded, not automatically detected.

### Register Mappings Across Forks: Shared Layouts And Exceptions

A recursive GitHub fork survey on 2026-09-10 found 72 accessible forks, including this one. Their default-branch EC register definitions were inspected, including renamed source files; gateslu's non-default `v1.0` branch was additionally checked. This is a source reference, not a supported-device list or an exhaustive audit of every branch and hardware access path. Identical definitions indicate shared source assumptions, not independent device confirmations or identical runtime behavior. External source examples are pinned to commits.

| Source-layout group | CPU / GPU temperature | CPU / GPU duty definitions | CPU / GPU RPM counter definitions | Forks |
| --- | --- | --- | --- | --- |
| Original single-fan layout | `0x07` / `0xCD` | `0xCE` / absent | `0xD0, 0xD1` / absent | 50 |
| Dual-fan `0xCD` layout, including this fork | `0x07` / `0xCD` | `0xCE` / `0xCF` | `0xD0, 0xD1` / `0xD2, 0xD3` | 15 |
| Dual-fan `0x0A` layout | `0x07` / `0x0A` | `0xCE` / `0xCF` | `0xD0, 0xD1` / `0xD2, 0xD3` | 2 |
| Dual duty definitions, CPU RPM only | `0x07` / `0xCD` | `0xCE` / `0xCF` | `0xD0, 0xD1` / absent | 2 |
| Partial GPU RPM implementation; see caveat below | `0x07` / `0xCD` definition | `0xCE` / absent | `0xD0, 0xD1` / only `0xD2` defined | 2 |
| CPU-only main daemon | `0x07` / absent | `0xCE` / absent | `0xD0, 0xD1` / absent | 1 |

Device evidence for these groups:

- **Dual-fan `0xCD`:** [This fork](src/clevo-indicator.c) is used on the development machine identified above. Clevo P775DM3 is named in the inherited [davidrohr](https://github.com/davidrohr/clevo-indicator/blob/6f85d0244359b011a10523ee9741fd41cc1af660/src/clevo-indicator.c#L67-L78) and [nicuessa](https://github.com/nicuessa/clevo-indicator/blob/4d69245ff2d31f085f6d1e593f25a3092f799558/src/clevo-indicator.c#L67-L78) source; hardware validation details are not documented.
- **Dual-fan `0x0A`:** gateslu's `v1.0` [README](https://github.com/gateslu/clevo-indicator/blob/91ec5041eebdd97b2507c8f9c99ef631b68d947d/README.md) explicitly allowlists COLORFUL X17 Pro Max. [Seb-sti1's implementation](https://github.com/Seb-sti1/clevo-indicator/blob/06c06c8d416747841c0dbdffa225720db19d7624/src/clevo-indicator.c#L73-L81) targets Gigabyte G5 MF5 2024; its [README](https://github.com/Seb-sti1/clevo-indicator/blob/06c06c8d416747841c0dbdffa225720db19d7624/README.md) tentatively identifies it as rebranded Clevo RC555 and reports GPU temperature verification against NVIDIA readings under load. These are author-reported targets, not devices independently tested by this project.
- **Original single-fan:** [SkyLandTW's implementation](https://github.com/SkyLandTW/clevo-indicator/blob/67facb8ebb3a5b098f11618205e4f42353c6981e/src/clevo-indicator.c#L67-L72) supplies the baseline layout; its README does not establish a specific model.

Duty addresses above are telemetry/readback locations, **not instructions to write directly to those registers**. RPM entries identify the two-byte counters used to calculate speed (high byte, low byte), not raw RPM values. Matching temperature addresses does not establish compatible fan commands, scaling, safe minimum duties, or firmware behavior. “Absent” means absent from that implementation, not absent from the hardware.

<details>
<summary>Fork membership by layout (GitHub owner names)</summary>

Owner names below identify the surveyed repositories in the SkyLandTW fork network; renamed repositories are called out explicitly. The pinned examples establish each layout, while membership records the default-branch snapshot on the review date.

- **Original single-fan layout:** Cotix, GuilloOme, HarrisonGregg, MrPiggy, NotesOfReality, OnnoBH, Soropureiya, WanQingGit, Wiimpathy, acidburn0zzz, adnan-alhomssi, alxb421, bradmccormack, brucej83, brunoais, cardozogp, codepeak, cuantar, cympfh, dainank, dbeniamine, dbtdsilva, digideskio, draekko, dszryan, eightone0, eliotlim, fouvy, gdoteof, gilvbp, hphilm, huaixv, idqaq, karamanh, kmwhite, littledragonblue, ljkgpxs, magavdraakon, ocrastroav, pedroguima, pnquan90, sathishkumark27, starlitxiling, superdan301246, sviande, taner1, theangrydev, wcasanova, yangzhl, yazawaniconii. These share the register set in the pinned SkyLandTW source above. Renamed repositories include `alxb421/cyrex-fanconrol`, `dainank/clevo-indicator-arch`, `dbtdsilva/clevo-indicator-sensors`, and the `system76-galapago-pro-fan-unfucker` repositories owned by Wiimpathy, cympfh, and gdoteof.
- **Dual-fan `0xCD` layout:** Brainiarc7, DiarmuidKelly, KediYamuk, PolicyChanges, davidrohr, devandrepascoa, ignatremizov, jacobmischka, jadetr, jcmonteiro, kepelrs, nicuessa, velaar, viridian1138, wmdhs12138. See the pinned davidrohr/nicuessa sources above. Matching constants do not mean the EC is always the selected temperature source: [DiarmuidKelly](https://github.com/DiarmuidKelly/clevo-indicator/blob/8c26abeaf1da2f52ccad586430dbd42a7b5280bc/src/clevo-indicator.c) also uses NVIDIA NVML.
- **Dual-fan `0x0A` layout:** Seb-sti1 and gateslu. See the model evidence above; [gateslu's default branch](https://github.com/gateslu/clevo-indicator/blob/a5438f7a01189dcc67ec99f2c0eb0441abe8398f/src/clevo-indicator.cpp) and its separately reviewed `v1.0` branch use the same eight addresses.
- **Dual duty definitions, CPU RPM only:** [comexr/comexr-fan-control-he](https://github.com/comexr/comexr-fan-control-he/blob/da4aaa4b63a6f7cdd5429387d966e1eb631ab54a/src/comexr-fan-control-he.c) and [digitalsparky/comexr-fan-control-he](https://github.com/digitalsparky/comexr-fan-control-he/blob/b28179ef5eeac62aad4d5dbae22df8187668ff1e/src/comexr-fan-control-he.c). Defining GPU duty does not provide a GPU RPM counter or guarantee that the UI reads both duties.
- **Partial GPU RPM implementation:** [bob912](https://github.com/bob912/clevo-indicator/blob/f66edfccb51f70508596b81cbe32eba7f081d8d4/src/clevo-indicator.c) and [hakanyorulmaz](https://github.com/hakanyorulmaz/clevo-indicator/blob/596ac32612e62d4e803726aab396de0f38df9e9e/src/clevo-indicator.c). Both pass `0xD2` data as both bytes of their GPU RPM calculation rather than defining a separate low byte. This is a source-level anomaly, not evidence for a different validated counter layout. bob912 also comments that `0xCD` appears to return zero after late-2019 updates and bypasses it in the GPU temperature accessor.
- **CPU-only main daemon:** [jhstatewide/system76-galapago-pro-fan-unfucker](https://github.com/jhstatewide/system76-galapago-pro-fan-unfucker/blob/2ff1d8c1289c67c83577de6bb925e4175da8b759/src/clevo-daemon.c). Its main daemon uses CPU temperature and one fan; retained diagnostic/test sources still contain `0xCD`, which should not be mistaken for active GPU control support.

</details>

The broad pattern is therefore two common GPU temperature addresses, not dozens of independently established device maps. Inherited constants, inactive GPU sensors, external NVIDIA readings, and partial implementations all make a repository count a poor measure of hardware compatibility.

When reporting another device, include its exact model and BIOS/EC firmware versions, the source revision, and how each reading was validated. Keep uncertain mappings explicitly marked as unverified; do not probe unknown writes or run competing EC control tools to fill in the table.

## Build

Install the build dependencies required by the build:

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libayatana-appindicator3-dev
```

Build the project:

```bash
git clone https://github.com/gzzchh/clevo-indicator
cd clevo-indicator
make
```

On distributions other than Ubuntu/Debian, install packages that provide these `pkg-config` targets:

- `gtk+-3.0`
- `ayatana-appindicator3-0.1`

## Hardware-Free Regression Tests

```bash
make check
make -B check TEST_SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'
```

These tests do not require root, access the EC/NVIDIA hardware, initialize GTK, launch the indicator, or change fan settings. They use mocked EC reads/port I/O, the real worker loop, and disposable subprocess fixtures for GPU-query tests. The historical `make test` target is **not** a test suite: it changes the built binary's ownership and setuid permissions.

## Indicator Polling And Fault Handling

- The worker reads eight EC registers using two positional reads: CPU temperature at `0x07`, then GPU temperature, duties, and RPM counters at `0xCD–0xD3`. The full 256-byte dump remains available only through `dumpall`.
- The existing 200 ms sleep after each cycle and normal automatic fan curve are unchanged. Reads and writes add their own latency; this is not a fixed 5 Hz deadline. Eight instead of 256 bytes means 96.9% fewer register reads **per sample**.
- Live before/after validation on the development machine measured `irq/9-acpi` CPU usage falling from about 10% to 0.6–0.8% of one core, worker CPU from about 2.33% to 0.2–0.4%, and EC interrupts from about 1,491 to 80 per second. Power draw also decreased, but the attributable power savings and battery-life improvement were not measured under controlled conditions. These are observations on this machine, not guaranteed results for other hardware or workloads.
- A partial/failed EC sample is not published. Previously valid temperatures expire after three seconds. Values outside 15–125°C are treated as invalid; unavailable temperatures are exposed as `-1`, including in the panel label.
- NVIDIA queries use `/usr/bin/nvidia-smi` without a shell, have a 1.5-second deadline, and invalidate their cached temperature on failure. Expired or invalid NVIDIA data falls back to fresh EC GPU data. The query thread waits one second between attempts. If a killed query remains stuck in the driver, the thread does not spawn a replacement until that child exits.
- If either temperature is unavailable, fans explicitly selected as **AUTO** request 100% until valid readings return. Manual fan settings are not overridden. Fault/recovery messages are logged on transitions. This is a conservative sensor-failure policy, not a change to the normal fan curve.
- Failed readback holds ordinary automatic adjustments; emergency requests remain possible. A fan command is confirmed only by a fresh duty readback, with one percentage point of tolerance for ordinary targets and exact 100% for emergency targets. Unconfirmed requests are retried at most once every two seconds per fan; a new 100% escalation can bypass that delay.
- Port-protocol timeouts stop the command rather than sending subsequent bytes after a failed wait. Repeated attempts are rate-limited, not abandoned after a finite retry count, because the requested cooling may still be needed.

The kernel's [`ec_sys` read implementation](https://github.com/torvalds/linux/blob/master/drivers/acpi/ec_sys.c) performs an EC transaction for each requested byte. This is why reducing the register range matters even though the old code used a single `read()` call.

### Validation Before Deployment

Building and running `make check` does not replace the installed indicator. Live deployment must be a separate, approved operation: preserve the installed binary for rollback, record current modes/duties, install the candidate, and gracefully restart only the indicator at an agreed time. Verify temperatures, duties, controls, `gpe6E` rate, and `irq/9-acpi` CPU against a baseline under a comparable workload. Do not mask the embedded controller's interrupt.

The worker still relies on kernel EC calls completing and on the existing Clevo-specific direct-port write protocol. These changes do not add a hardware watchdog, coordinate with other EC writers, or guarantee recovery from a stalled controller/GPU driver. Do not run another EC control tool concurrently.

## Install

Install the binary with the setuid-root mode required for EC access:

```bash
sudo make install
```

The install target places the binary at:

```text
/usr/local/bin/clevo-indicator
```

## Run

Use the CLI commands directly:

```text
clevo-indicator set [fan-duty-percentage]
clevo-indicator setg [fan-duty-percentage]
clevo-indicator dump
clevo-indicator dumpall
clevo-indicator auto
clevo-indicator indicator
clevo-indicator help
```

Use `fan-duty-percentage` as an integer percentage value.

Examples:

```bash
clevo-indicator dump
clevo-indicator set 70
clevo-indicator setg 80
clevo-indicator auto
/usr/local/bin/clevo-indicator indicator
```

## Use The GNOME Panel UI

Run the indicator with:

```bash
/usr/local/bin/clevo-indicator indicator
```

Expect the default UI to provide:

- one combined top-bar label
- one custom popup window
- separate CPU and GPU control columns
- presets `AUTO`, `40%`, `50%`, `60%`, `70%`, `80%`, `90%`, `100%`

### Install The Required GNOME Host Fork

Use the patched GNOME AppIndicators host extension. Stock GNOME AppIndicators do not provide the required behavior for this UI.

Required host behavior:

- hide the icon actor completely
- route primary click to `Activate(x, y)` instead of always opening DBusMenu

Use this fork:

- GitHub: `git@github.com:ignatremizov/gnome-shell-extension-appindicator.git`
- local clone: `~/code/gnome-shell-extension-appindicator`
- local extension UUID: `appindicatorsupport@ignatremizov.com`
- local install path:
  `~/.local/share/gnome-shell/extensions/appindicatorsupport@ignatremizov.com`

Treat the single-indicator UI as a two-part system:

1. export a native `StatusNotifierItem` from this repo
2. consume `XClevoShowIcon=false` and `XClevoPreferActivate=true` in the patched GNOME host

Read [docs/SNI_HOST_PATCH.md](docs/SNI_HOST_PATCH.md) for the host-side details.

### Force The Legacy UI

Force the dual-AppIndicator fallback for compatibility or debugging:

```bash
CLEVO_LEGACY_APPINDICATOR=1 /usr/local/bin/clevo-indicator indicator
```

Use that mode when the patched GNOME host fork is unavailable or when you want to compare the old flat-menu behavior.

## Privilege Model And Safety

Run the indicator as the desktop user. Let the installed setuid bit supply the root privileges required for EC access.

The binary must do both of these jobs:

- run UI code as the desktop user so GNOME can show the indicator
- access Clevo EC interfaces with root privileges

That split causes the process tree to fork into a UI side and a privileged worker side. Killing either process will terminate the other.

Do not run other EC-tweaking tools at the same time. This project does not coordinate low-level EC access across multiple processes.

Avoid `kill -9` unless there is no other recovery path.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) for workflow and commit-message conventions.

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the UI and worker design.

## Hacking

Edit `control_rows[]` in [src/clevo-indicator.c](src/clevo-indicator.c) to change the CPU/GPU preset list exposed in the popup and legacy menus.

Preset table:

```c
static FanControlRow control_rows[] = {
    {"AUTO", 0, AUTO, NULL, NULL, NULL, NULL},
    {"40%",  40, MANUAL, NULL, NULL, NULL, NULL},
    {"50%",  50, MANUAL, NULL, NULL, NULL, NULL},
    {"60%",  60, MANUAL, NULL, NULL, NULL, NULL},
    {"70%",  70, MANUAL, NULL, NULL, NULL, NULL},
    {"80%",  80, MANUAL, NULL, NULL, NULL, NULL},
    {"90%",  90, MANUAL, NULL, NULL, NULL, NULL},
    {"100%", 100, MANUAL, NULL, NULL, NULL, NULL},
};
```

Edit `ec_auto_duty_adjust()` in [src/clevo-indicator.c](src/clevo-indicator.c) to change the automatic duty curve.
