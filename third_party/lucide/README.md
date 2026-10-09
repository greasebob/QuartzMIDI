# Lucide

Source SVGs for the shell's icons and the YouTube window's. Lucide v1.41.0, ISC, see `LICENSE`.

Downloaded from `https://cdn.jsdelivr.net/npm/lucide-static@1.41.0/icons/<name>.svg`.
Only the icons in use are kept; `tools/gen-icons.py` lists which Lucide file
backs which `Icon` enumerator and regenerates `ui/IconData.hpp` from them, and
its `WEB_ICONS` list writes `tools/mp3-to-midi/page/icons.svg`.

The shell's SVGs are not read at runtime: they are flattened to polylines at
generation time. The converter add-on ships `page/icons.svg`, a sprite of the
`WEB_ICONS` files, and the release ships the licence beside both.
