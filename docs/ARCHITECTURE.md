# clevo-indicator – Architecture & Design Findings

## Problem statement

Show a live `C:50°C 60%  G:47°C 60%` label in the GNOME panel bar **and** let the user click once to get a **two-column** CPU Fan / GPU Fan control popup.

## Why this is hard on modern GNOME

### The three tray protocols

| Protocol | How it works | Label in bar | Custom widgets in popup |
|---|---|---|---|
| **XEmbed** (legacy) | App embeds a tiny window in the panel via the System Tray spec | ❌ icon only, forced to a square | ✅ full GTK widget tree shown in-process |
| **AppIndicator / KStatusNotifierItem** | App exports a D-Bus service; GNOME extension reads it | ✅ `app_indicator_set_label()` | ❌ menu serialised over D-Bus, custom widgets stripped |
| **libdbusmenu** | Menu serialisation layer used by AppIndicator | n/a | ❌ only plain label strings survive D-Bus round-trip |

### XEmbed details

`GtkStatusIcon` uses the XEmbed protocol.  The GNOME Shell extension `ubuntu-appindicators@ubuntu.com` wraps it as an `IndicatorTrayIcon` and forces the icon to be **square** (`width = iconSize, height = iconSize` where `iconSize` defaults to 22 px).  Rendering text into a 220×22 pixbuf and setting it as the icon produces an unreadable waveform because the extension squishes it to 22×22.

### AppIndicator / dbusmenu details

`app_indicator_set_label()` works perfectly – the GNOME extension reads the label string over D-Bus and renders it in the panel bar.

However, the menu is sent through `libdbusmenu`.  On our side, `libdbusmenu-gtk3` serialises the GtkMenu tree to D-Bus items.  On the GNOME Shell side, `DBusMenu.Client` recreates **its own** St/Clutter menu items from those D-Bus descriptions – only the `label`, `icon-name`, `visible`, `enabled`, and a handful of other primitive properties are round-tripped.  Any custom GtkWidget children (GtkHBox, GtkLabel, GtkButton…) are **silently discarded**.  A two-column layout built from nested GTK widgets never appears.

`DbusmenuServer` does fire an `item-activation-requested` signal in our process when the user activates a dbusmenu item, but that is still a two-step interaction (click indicator → click menu item → second popup), which the user rejected.

`libappindicator` / `libayatana-appindicator` exposes no public `activate` or `click` signal we can hook to intercept the *first* click on the indicator icon itself.  The underlying StatusNotifierItem protocol does define `Activate`, `SecondaryActivate`, and `ContextMenu`, but in GNOME Shell's AppIndicator extension the first primary click is used to open the menu; `Activate` is only used for double-click when supported, and middle-click maps to `SecondaryActivate`.

### GtkMenu grab problem

A `GtkMenu` shown via `gtk_menu_popup_at_pointer()` calls `gdk_seat_grab()` on X11, taking an **exclusive seat grab** (pointer + keyboard).  While the grab is active:
- All keyboard events are delivered only to the grab window.
- Global hotkeys wired up at the compositor level (PrintScreen → screenshot, Super → Activities, …) are silenced.
- The tray icon stays in the `active` pseudo-class because the button-release event that would normally clear it never reaches the GNOME extension actor (the grab has stolen it).

`gtk_menu_shell_set_take_focus(FALSE)` affects only keyboard *focus*, not the seat grab itself; it does not help.

## Chosen design

### Two simultaneous tray entries

```
[ fan-icon ]  [ C:90°C 70%  G:69°C 80% ▾ ]
     ↑                    ↑
GtkStatusIcon         AppIndicator
(XEmbed)          (KStatusNotifierItem)
```

- **AppIndicator** is registered just for the label.  Its dbusmenu menu contains only a greyed-out status line and a Quit item.  `app_indicator_set_label()` is called every 500 ms to update the text in the panel bar.

- **GtkStatusIcon** (XEmbed) handles clicks.  Left-click and right-click both call `ui_popup_show()`, which positions and shows `fan_popup_window`.

### Grab-free popup window

`fan_popup_window` is a `GTK_WINDOW_TOPLEVEL` with:

```c
gtk_window_set_decorated(FALSE);                          // no title bar
gtk_window_set_skip_taskbar_hint(TRUE);                   // invisible to alt-tab
gtk_window_set_skip_pager_hint(TRUE);
gtk_window_set_keep_above(TRUE);
gtk_window_set_type_hint(GDK_WINDOW_TYPE_HINT_POPUP_MENU); // WM treats as popup
```

`GDK_WINDOW_TYPE_HINT_POPUP_MENU` tells the WM and compositor to treat the window like a popup menu (no shadow chrome, above everything) **without** taking a seat grab.  Because there is no grab:

- Keyboard events continue to flow normally → PrintScreen works.
- The button-release event that closes the tray icon highlight still reaches the GNOME extension → no stuck highlight.

Dismissal is handled by:
- `focus-out-event` → `gtk_widget_hide()` (clicking elsewhere steals focus).
- `GDK_KEY_Escape` key press handler.
- Clicking a fan button (`ui_fan_btn_clicked`) hides the window and applies the fan setting.

### FanControlRow

```c
typedef struct {
    const char   *label;      // "AUTO", "40%", …
    int           duty;       // 0 = auto, 40–100 = manual %
    MenuItemType  type;       // AUTO or MANUAL
    GtkWidget    *cpu_btn;    // GtkButton in CPU column
    GtkWidget    *gpu_btn;    // GtkButton in GPU column
} FanControlRow;
```

`ui_toggle_menuitems()` prepends `•` to the button label whose duty matches the current setting, updated every 500 ms by `ui_update()`.

### IPC between UI and EC worker

The UI process forks a privileged EC worker child.  They share an `mmap(MAP_ANON | MAP_SHARED)` page.  The UI writes `manual_next_{cpu,gpu}_fan_duty` and reads back `cpu_temp`, `gpu_temp`, `cpu_fan_duty`, `gpu_fan_duty`.  The worker polls the EC at 200 ms and applies any pending duty changes.

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
