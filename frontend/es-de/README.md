# ES-DE / RetroDECK integration

Adds a **"GameTank"** system to ES-DE (and RetroDECK) so `.gtr` cartridges show up
as their own category, launched through `gametank-libretro`.

## Install

1. **Put the core where the frontend looks for cores.** For standard RetroArch
   that's the cores dir `%CORE_RETROARCH%` resolves to, and the `<command>` in
   `es_systems.snippet.xml` uses that variable directly. On RetroDECK the bundled
   cores dir is read-only in the flatpak, so keep the `.so` in a writable host dir
   and change `<command>` to an absolute `-L` path to the `.so` instead.

2. **Add the system** — merge the `<system>` block from `es_systems.snippet.xml`
   into ES-DE's custom systems file (this is the supported user-add path; ES-DE
   does not overwrite it on update — but see the #1335 note below):
   - Native ES-DE: `~/ES-DE/custom_systems/es_systems.xml`
   - RetroDECK: `~/retrodeck/ES-DE/custom_systems/es_systems.xml`

   It's a SNIPPET, not a whole file — if the file already has other systems, add
   our `<system>` inside the existing `<systemList>`, don't replace it.

3. **Add the logo** (optional but recommended) — drop `logos/gametank.svg` into the
   active theme's per-system logo dir. For art-book-next (RetroDECK default):
   ```
   ~/retrodeck/ES-DE/themes/art-book-next-es-de/_inc/systems/logos/gametank.svg
   ```
   (Theme files DO get overwritten on a theme update; re-copy after updating.)

4. **Put games** in `%ROMPATH%/gametank/` as `.gtr` files
   (`~/retrodeck/roms/gametank/`).

5. **Restart ES-DE/RetroDECK fully.** The systems config is read once at startup —
   "reload gamelist" is not enough; the new system only appears after a full restart.

## RetroDECK gotcha (#1335): es_systems.xml can reset on a flatpak update

The custom `es_systems.xml` can be wiped by a RetroDECK flatpak update. Keep a
backup of your merged file; if the GameTank system disappears after an update,
re-apply the snippet (or restore from the backup).

## Why `<theme>gametank</theme>` (not `pc` or `ports`)

ES-DE picks a system's displayed **name and logo from the theme**, keyed by the
`<theme>` value (`${system.theme}`), NOT from `<fullname>`. Pointing `<theme>` at an
existing system borrows *that* system's identity (`<theme>pc</theme>` → "IBM PC").
Using a name **no stock theme defines** (`gametank`) makes ES-DE fall back to our
`<fullname>` and our own dropped-in `logos/gametank.svg`.

## Logo gotcha: ES-DE renders SVGs with **nanosvg**, which ignores `<text>`

`logos/gametank.svg` must be built from **paths/shapes only** — `<text>` elements
are **not rendered** by ES-DE's SVG loader. Our logo is pixel-block letters drawn as
`<rect>`s, white-filled so the theme can recolor it via `${systemLogoColor}`.
Regenerate with `gen-logo.py` if the wordmark changes.

## glibc: no compat shim needed

The core's glibc floor is **2.14**, well below what current flatpak runtimes and
handheld distros ship — so a locally-built `.so` loads directly without any
glibc-compat wrapping. (Cores that statically link a heavy runtime can pull much
newer symbols, e.g. `GLIBC_2.43`, that must be `--wrap`ped down to load on an older
flatpak runtime; the lean C++ core sidesteps that whole problem.)
