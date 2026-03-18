# Contributing

## Commit message convention

This repository uses a conventional message format for all commits, based on recent history.

### Required format

- **Subject line**:
  - `type(scope): short imperative summary`
  - Example: `fix(indicator): align CPU fan output labels`
- **Body**:
  - Use heading blocks in this order:
    - `Why:`
    - `What changed:`
    - `Impact:`

### Allowed commit types

- `feat` — new feature
- `fix` — bug fix
- `chore` — maintenance tasks
- `refactor` — internal refactors without behavior change
- `build` — build/install or packaging changes
- `docs` — documentation updates

### Example templates

```text
Minimal example:

fix(indicator): align CPU fan output labels

Why:
- Keep fan telemetry labels consistent between CPU and GPU dumps.

What changed:
- Renamed `CPUFAN Duty` to `CPU FAN Duty` in `main_dump_fan`.
- Renamed `CPUFAN RPMs` to `CPU FAN RPMs` in `main_dump_fan`.

Impact:
- Improves readability of CLI output and makes script maintenance easier.
```

```text
Feature example:

feat(indicator): add temperature source selector for auto mode

Why:
- Some systems report GPU temperature only through EC and some through platform-specific tools.

What changed:
- Added CLI flag `--gpu-temp-source` with values `ec` and `nvidia-smi`.
- Default behavior remains unchanged for existing environments.
- Added validation and fallback logging when the selected source is unavailable.

Impact:
- Improves user control over sensor source and simplifies troubleshooting on mixed hardware.
```

```text
Refactor example:

refactor(appindicator): reduce repeated indicator action creation

Why:
- Menu initialization repeated similar blocks and was difficult to safely extend.

What changed:
- Introduced a helper that creates menu entries from a declarative descriptor.
- Simplified initialization order while keeping labels, callbacks, and menu behavior intact.
- Added one targeted helper for consistent icon/title setup.

Impact:
- Lowers risk of regressions when adding future menu items and reduces future maintenance work.
```

### Writing multiline messages safely

When writing commit messages that include backticks or multiple lines, prefer:

- `cat <<'EOF' | git commit -F -`
- Avoid using repeated `-m` for multiline body sections, since it inserts extra paragraph breaks.

Examples:

```bash
cat <<'EOF' | git commit -F - -- src/clevo-indicator.c
fix(indicator): ...

Why:
- ...

What changed:
- ...

Impact:
- ...
EOF
```
