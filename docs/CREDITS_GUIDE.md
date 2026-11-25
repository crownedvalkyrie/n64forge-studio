# Adding people to Credits

Credits are data-driven. The Studio reads `resources/credits/credits.json` at
startup (bundled into the app as `:/credits/credits.json`). The same data powers
the **Devs**, **Supporters** and **Testers** tabs.

## Four-line entry format

Add one object to the matching array (`devs`, `supporters` or `testers`):

```json
    {
      "name": "Ada Lovelace",
      "role": "Developer",
      "note": "Core scheduler and deterministic tests.",
      "pfp": "avatars/ada.png"
    }
```

- `name` is required. It is also the initials-badge fallback and the stable
  accent-color seed.
- `role` becomes the small pill next to the name.
- `note` is the one-line contribution description.
- `pfp` is optional. Leave it empty to use the initials badge.

## Adding a PFP

1. Drop a `.png` or `.jpg` into `resources/credits/avatars/`.
2. Set `"pfp"` to a path relative to `credits.json`, for example
   `"avatars/ada.png"`.
3. Absolute paths also work.

The Studio draws a `44×44` circular avatar with a subtle accent border. If the
file is missing or unreadable, it generates a deterministic colored initials
badge, so nobody is blocked on artwork.

## Rebuilding and local overrides

Rebuild Studio after editing the bundled JSON (`cmake --build --preset
macos-release`). For a quick local edit, either:

- place `credits.json` in `resources/credits/` next to the app, or
- set `N64FORGE_CREDITS_JSON` to an absolute path.

The Credits view also contains the same instructions under **How to Add People
& PFPs**. No separate settings page is added: Credits is a read-only view.
