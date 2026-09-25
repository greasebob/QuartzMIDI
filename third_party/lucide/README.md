# Lucide

Source SVGs for the shell's icons. Lucide v1.41.0, ISC, see `LICENSE`.

Downloaded from `https://cdn.jsdelivr.net/npm/lucide-static@latest/icons/<name>.svg`.
Only the icons the shell uses are kept; `tools/gen-icons.py` lists which Lucide
file backs which `Icon` enumerator and regenerates `ui/IconData.hpp` from them.

The SVGs are not read at runtime. They are flattened to polylines at generation
time, so nothing here ships with the executable except the licence.
