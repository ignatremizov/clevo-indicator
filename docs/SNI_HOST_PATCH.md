# SNI Host Patch Plan

This repo now contains a native `StatusNotifierItem` scaffold in:

- `src/sni.h`
- `src/sni.c`

It exports the following non-standard properties for the GNOME host fork to
consume:

- `XClevoShowIcon` (`b`)
- `XClevoPreferActivate` (`b`)

The intended host-side behavior is:

1. If `XClevoShowIcon == false`, do not create or add an icon actor to the
   indicator box.
2. If `XClevoPreferActivate == true`, primary click should call
   `Activate(x, y)` instead of opening the DBusMenu menu.
3. Keep right-click mapped to the normal DBusMenu path for fallback actions.

## GNOME AppIndicators host fork

Primary fork for this work:

- GitHub: `git@github.com:ignatremizov/gnome-shell-extension-appindicator.git`
- Local clone: `~/code/gnome-shell-extension-appindicator`
- Local installed extension UUID/path:
  `~/.local/share/gnome-shell/extensions/appindicatorsupport@rgcjonas.gmail.com`

The Ubuntu-packaged extension remains useful as a reference when comparing the
stock behavior:

- `/usr/share/gnome-shell/extensions/ubuntu-appindicators@ubuntu.com/`

Relevant files:

- `indicatorStatusIcon.js`
- `appIndicator.js`
- `interfaces-xml/StatusNotifierItem.xml`

### `interfaces-xml/StatusNotifierItem.xml`

Add the two custom properties to the proxy interface:

- `XClevoShowIcon`
- `XClevoPreferActivate`

### `appIndicator.js`

Expose getters that return those properties, for example:

- `get clevoShowIcon()`
- `get clevoPreferActivate()`

If the extension is later switched to support activation-only indicators,
`_checkIfReady()` can be relaxed from:

```js
if (this.hasNameOwner && this.id && this.menuPath)
    isReady = true;
```

to something like:

```js
if (this.hasNameOwner && this.id &&
    (this.menuPath || this.clevoPreferActivate))
    isReady = true;
```

### `indicatorStatusIcon.js`

Two changes are needed:

1. Skip creating/adding the icon actor when `indicator.clevoShowIcon === false`
2. In `vfunc_button_press_event`, route primary click to
   `this._indicator.open(...event.get_coords(), event.get_time())` when
   `indicator.clevoPreferActivate === true`

The current extension already supports `Activate` and XDG activation tokens, so
the host patch is only changing click routing and actor layout.

## Current repo integration

`CLEVO_EXPERIMENTAL_SNI=1` enables the native SNI scaffold without changing the
default AppIndicator runtime path. The current scaffold:

- exports label updates
- exports `XClevoShowIcon=false`
- exports `XClevoPreferActivate=true`
- logs `Activate` / `ContextMenu` / `SecondaryActivate` calls

The custom popup window is not wired yet; that becomes the next app-side step
once the host extension is patched to route primary click via `Activate`.
