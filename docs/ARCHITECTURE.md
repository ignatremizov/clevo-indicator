# clevo-indicator Architecture

## Problem statement

`clevo-indicator` exposes live CPU and GPU temperature and fan duty in the GNOME panel and lets the user change CPU and GPU fan presets from the tray with a single click.

The current UI intentionally uses two independent indicators:

```text
[ 🌀 C:90° 70% ] [ G:69° 80% 🌀 ]
     CPU menu          GPU menu
```

Each indicator owns one flat native menu for its fan, so the interaction is:

1. Click CPU indicator, choose a CPU fan preset.
2. Click GPU indicator, choose a GPU fan preset.

This replaced the earlier hybrid design that mixed a `GtkStatusIcon` click target with a custom popup window.

## High-level design

The process splits into two halves:

- The unprivileged GTK UI process owns the `AppIndicator` instances, panel labels, and menu interaction.
- A privileged EC worker loop reads the embedded controller, applies pending fan changes, and keeps shared state fresh.

They communicate through one shared anonymous `mmap` page (`share_info`), which contains:

- current temperatures
- current fan duty and RPM values
- auto/manual mode flags
- pending manual duty requests
- the last duty values applied by the worker

## Why the UI uses two AppIndicators

Modern GNOME tray integration has two constraints that drive the design:

- `GtkStatusIcon` is legacy XEmbed and is deprecated and unreliable on modern GNOME.
- `AppIndicator` menus are serialized through dbusmenu, so only simple native menu items survive; custom GTK widget layouts do not.

### The three tray protocols

| Protocol | How it works | Label in bar | Custom widgets in popup |
|---|---|---|---|
| **XEmbed** (legacy) | App embeds a tiny window in the panel via the System Tray spec | ❌ icon only, forced to a square | ✅ full GTK widget tree shown in-process |
| **AppIndicator / KStatusNotifierItem** | App exports a D-Bus service; GNOME extension reads it | ✅ `app_indicator_set_label()` | ❌ menu serialised over D-Bus, custom widgets stripped |
| **libdbusmenu** | Menu serialisation layer used by AppIndicator | n/a | ❌ only plain label strings survive D-Bus round-trip |

### XEmbed details

`GtkStatusIcon` uses the XEmbed protocol. The GNOME Shell extension `ubuntu-appindicators@ubuntu.com` wraps it as an `IndicatorTrayIcon` and forces the icon to be square (`width = iconSize, height = iconSize` where `iconSize` defaults to 22 px). Rendering text into a wide pixbuf and setting it as the icon produces an unreadable squashed result because the extension compresses it back to 22×22.

### AppIndicator / dbusmenu details

`app_indicator_set_label()` works because the GNOME extension reads the label string over D-Bus and renders it directly in the panel bar.

The menu path is more constrained. On our side, `libdbusmenu-gtk3` serialises the `GtkMenu` tree into D-Bus menu items. On the GNOME Shell side, `DBusMenu.Client` reconstructs its own menu items from those D-Bus descriptions. Only primitive properties survive that round-trip:

- `label`
- `icon-name`
- `visible`
- `enabled`
- a small set of similar metadata

Custom GTK widget children do not survive. Nested `GtkBox`, `GtkGrid`, `GtkLabel`, `GtkButton`, and similar widget structures are discarded during menu serialisation, which is why a two-column in-process layout cannot appear inside an AppIndicator menu on GNOME.

`libappindicator` / `libayatana-appindicator` exposes no public `activate` or `click` signal we can hook to intercept the *first* click on the indicator icon itself.  The underlying StatusNotifierItem protocol does define `Activate`, `SecondaryActivate`, and `ContextMenu`, but in GNOME Shell's AppIndicator extension the first primary click is used to open the menu; `Activate` is only used for double-click when supported, and middle-click maps to `SecondaryActivate`.

The previous architecture tried to combine both worlds:

- `AppIndicator` for panel text
- `GtkStatusIcon` for click handling
- a custom popup window for the two-column CPU/GPU control surface

That design was removed because it was fragile and unpleasant in practice:

- custom popup placement depended on pointer-position heuristics
- global keyboard actions could be disrupted while the popup was open
- Wayland-forwarded and compositor-specific behavior was brittle
- the code had to maintain two tray protocols for one feature

The current design accepts dbusmenu's flat-menu constraint and models CPU and GPU as separate indicators instead.

## Previous approach

Before the current dual-`AppIndicator` design, the UI used a hybrid tray architecture to satisfy two conflicting requirements:

- show a live text label in the panel bar
- open a custom two-column CPU/GPU fan control surface on the first click

### Two simultaneous tray entries

The previous design created two different tray objects for different jobs:

```text
[ fan-icon ]  [ C:90°C 70%  G:69°C 80% ▾ ]
     ↑                    ↑
GtkStatusIcon         AppIndicator
(XEmbed)          (KStatusNotifierItem)
```

- `AppIndicator` existed only to expose the live combined `C:...  G:...` text label in the GNOME panel.
- `GtkStatusIcon` existed only to receive clicks, because dbusmenu could not express the intended two-column control layout.

The AppIndicator menu contained only:

- a read-only status item
- a separator
- `Quit`

The actual fan controls lived outside the AppIndicator menu.

### Grab-free popup window

Clicking the `GtkStatusIcon` opened `fan_popup_window`, a custom undecorated `GTK_WINDOW_TOPLEVEL` styled like a popup menu:

```c
gtk_window_set_decorated(GTK_WINDOW(fan_popup_window), FALSE);         // no title bar
gtk_window_set_skip_taskbar_hint(GTK_WINDOW(fan_popup_window), TRUE);  // invisible to alt-tab
gtk_window_set_skip_pager_hint(GTK_WINDOW(fan_popup_window), TRUE);
gtk_window_set_keep_above(GTK_WINDOW(fan_popup_window), TRUE);
gtk_window_set_type_hint(GTK_WINDOW(fan_popup_window),
                         GDK_WINDOW_TYPE_HINT_POPUP_MENU);             // WM treats as popup
```

`GDK_WINDOW_TYPE_HINT_POPUP_MENU` tells the WM and compositor to treat the window like a popup menu (no shadow chrome, above everything) **without** taking a seat grab.  Because there is no grab:

- Keyboard events continue to flow normally → PrintScreen works.
- The button-release event that closes the tray icon highlight still reaches the GNOME extension → no stuck highlight.

That window contained a two-column grid:

- CPU Fan column
- GPU Fan column

Each row exposed the same preset on both sides, for example `AUTO`, `40%`, `50%`, and so on. The popup was manually shown near the current pointer position.

Dismissal was handled by:

- `focus-out-event` → `gtk_widget_hide()` (clicking elsewhere steals focus).
- `GDK_KEY_Escape` key press handler.
- Clicking a fan button (`ui_fan_btn_clicked`) hides the window and applies the fan setting.

This approach was chosen because a real `GtkMenu` would take a seat grab on X11 and interfere with global keyboard actions such as PrintScreen or Super.

### Previous `FanControlRow`

The old fan control table stored button widgets for the popup grid:

```c
typedef struct {
    const char   *label;      // "AUTO", "40%", …
    int           duty;       // 0 = auto, 40–100 = manual %
    MenuItemType  type;       // AUTO or MANUAL
    GtkWidget    *cpu_btn;    // GtkButton in CPU column
    GtkWidget    *gpu_btn;    // GtkButton in GPU column
} FanControlRow;
```

`ui_toggle_menuitems()` updated those button labels to prepend `•` to the currently selected duty.

### Why it was replaced

The hybrid design worked around protocol limitations, but the cost was too high:

- it depended on deprecated `GtkStatusIcon`
- it required maintaining two separate tray integrations for one feature
- popup placement depended on manual cursor tracking and compositor behavior
- the custom popup path was fragile across GNOME setups and Wayland-forwarded sessions
- keyboard and shell interactions were still easy to make feel wrong while the popup was open
- the architecture was more complex than the user interaction justified

Once the project accepted flat native menus instead of a two-column popup, the simpler replacement became obvious:

- create one native `AppIndicator` for CPU
- create one native `AppIndicator` for GPU
- put each fan's presets directly in that indicator's menu
- keep label rendering and click handling within the same tray protocol

That is the architecture described in the sections below.

## UI architecture

### Indicator layout

`main_ui_worker()` creates the indicators in right-to-left panel order:

- GPU indicator first, so it renders on the right
- CPU indicator second, so it renders on the left

Both indicators are `AppIndicator` instances with:

- status `APP_INDICATOR_STATUS_ACTIVE`
- their own `GtkMenu`
- their own label string
- a shared icon theme path generated at startup

The GPU indicator label format is:

```text
G:<temp>° <duty>% 🌀
```

The CPU indicator label format is:

```text
🌀 C:<temp>° <duty>%
```

This split keeps each indicator self-contained and avoids the need for a popup submenu or custom widget layout.

### Menu model

The fan presets are defined once in `control_rows[]`:

```c
AUTO, 40%, 50%, 60%, 70%, 80%, 90%, 100%
```

Each row stores pointers to:

- the CPU menu item for that preset
- the GPU menu item for that preset

Selecting a menu item calls `ui_fan_item_activated()`, which delegates to `ui_command_set_fan()`. That function updates shared state only:

- choosing `AUTO` sets `auto_{cpu,gpu}_duty = 1` and clears the pending manual duty
- choosing a manual percentage disables auto mode and stores `manual_next_{cpu,gpu}_fan_duty`

The UI does not write EC registers directly.

### Menu selection state

`ui_toggle_menuitems()` refreshes menu labels so the active preset is marked with a leading bullet.

The selected state is derived from shared state:

- auto mode selects `AUTO`
- pending manual requests win over the last applied manual value
- otherwise the worker's last applied manual duty is shown

This lets the menu reflect a user click immediately, before the worker loop finishes another EC cycle.

### Label refresh path

`ui_update()` runs every 500 ms via `g_timeout_add()`. It:

1. Computes the displayed CPU and GPU duties, preferring pending manual values.
2. Builds a combined cache key in `ui_last_label`.
3. Pushes separate label strings to `indicator_cpu` and `indicator_gpu` only when the visible state changed.
4. Refreshes menu selection markers.

The UI uses `°` instead of `℃` in all panel text to avoid font support issues.

### Reconnect handling

Both indicators subscribe to the `connection-changed` signal.

When the notifier host disconnects and reconnects, `ui_on_reconnect()` clears `ui_last_label`, forcing the next `ui_update()` tick to republish the labels. This avoids stale or missing panel text after host restarts.

### Runtime icon theme

AppIndicator expects icon names, not arbitrary in-memory pixbufs. To support label-first indicators without shipping static assets, `ui_setup_icon_theme()` creates a minimal icon theme in `/tmp/clevo-indicator-icons` at startup.

It writes:

- `hicolor/index.theme`
- `clevo-blank.png`, a transparent 22x22 icon
- `clevo-fan.png`, a drawn fan icon
- `clevo-cyclone.png`, another drawn fan icon asset

Both indicators currently use the generated blank icon so the panel shows the label text cleanly. The fan glyph shown in labels comes from the label string itself.

`ui_draw_fan_icon()` uses Cairo to render the icon assets.

## EC worker architecture

### Responsibility split

`main_ec_worker()` runs as root and is responsible for:

- loading `ec_sys`
- opening `/sys/kernel/debug/ec/ec0/io`
- reading the 256-byte EC register space
- applying pending manual CPU and GPU fan duty changes
- running the auto-duty adjustment logic
- updating the shared `share_info` page

The loop runs every 200 ms.

### EC sysfs I/O

The worker uses low-level file-descriptor I/O:

- `open()`
- `lseek(fd, 0, SEEK_SET)`
- `read()`
- `close()`

This replaced `fopen()`/`fread()`/`rewind()`/`fclose()`. The explicit file-descriptor path avoids stdio buffering interactions when re-reading the same EC sysfs node every cycle.

### GPU temperature sampling

When `use_gpu_temp_smi` is enabled, the worker starts a detached `pthread` running `gpu_temp_smi_thread()`.

That thread:

- calls `ec_query_gpu_temp_nvidia()`
- stores the latest successful result in `g_gpu_temp_smi`
- sleeps for one second between samples

The EC loop then chooses GPU temperature as:

- latest SMI sample if one is available
- otherwise the EC register value

This decouples Nvidia SMI polling from the 200 ms EC hot loop and gives the SMI path a steady 1 Hz sampling rate.

### Auto mode

The worker computes a single `target_temp` as:

```c
MAX(cpu_temp, gpu_temp)
```

If CPU or GPU auto mode is enabled, it adjusts that fan using `ec_auto_duty_adjust()` and writes a new duty only when the computed target changes.

This keeps both automatic fan controllers keyed off the hottest component.

## Data flow

The steady-state flow is:

1. UI timer reads `share_info` and updates both indicator labels and menu markers.
2. User activates a CPU or GPU menu item.
3. UI writes the requested mode or duty into `share_info`.
4. EC worker sees the pending change on the next cycle and writes it to the controller.
5. EC worker refreshes temperatures, fan duty, and RPM values from hardware.
6. UI reflects the new state on the next timer tick.

## Build impact

The architecture now requires POSIX threads for the GPU SMI polling thread, so the build links with `-lpthread`.

## Alternative approaches considered and rejected

| Approach | Why rejected |
|---|---|
| Pure GtkStatusIcon + text-as-pixbuf | Panel squishes pixbuf to a 22×22 square; text is unreadable waveform |
| Pure AppIndicator + GtkMenu via dbusmenu | dbusmenu strips custom widget children; two-column layout never appears |
| AppIndicator + `item-activation-requested` signal | Still two-step: click indicator → click "Fan Control…" → popup |
| AppIndicator menu with `gtk_menu_shell_set_take_focus(FALSE)` | Prevents focus grab only; seat grab and keyboard interception remain |
| `GtkMenu` + `gdk_seat_ungrab` immediately after popup | Releases pointer grab too, so clicking outside no longer closes menu |
| Single AppIndicator with flat two-section menu | Works in one click and shows text, but no two-column layout (user requirement) |
| `GtkPopover` anchored to status icon | GtkPopover cannot anchor to a GtkStatusIcon (not a GtkWidget); requires Wayland xdg-popup which has its own grab semantics |

## Tradeoffs and non-goals

The current design makes a few explicit tradeoffs:

- It prefers native one-column menus over a custom two-column popup.
- It uses two tray entries instead of one composite indicator.
- It writes ephemeral icon assets into `/tmp` instead of installing theme assets system-wide.

Those tradeoffs are deliberate. The design is simpler, works with the tray protocol GNOME actually supports today, and removes the deprecated `GtkStatusIcon` dependency entirely.
