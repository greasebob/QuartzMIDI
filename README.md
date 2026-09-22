# QuartzMIDI

QuartzMIDI plays MIDI files on a game's piano by typing the notes into the active window, and plays that piano live from a MIDI keyboard.

By BobGrease. Based on [MIDI++](https://github.com/Zephkek/MIDIPlusPlus) by Zephkek.

## Features

- Autoplay on 61 or 88 keys, with velocity, sustain, speed and transpose.
- A MIDI library with folders, search, shuffle, and hotkeys that work from inside the game.
- Tracks with mute, solo and Solo Piano.
- The game's key maps: Major, Dorian, Lydian, Locrian, Mixolydian, Minor, Phrygian and Phrygian Dom.
- Live play from a MIDI keyboard or a Wooting analog keyboard, over Kernel Streaming or WinMM.
- MIDI output to a port instead of typed keys.
- A velocity curve editor and AutoVol.
- Themes with light and dark palettes, an editor, Import and Export.
- A mini window, Always on top, media keys, Help and a first-start tour.

## Add-ons

`QuartzMIDI-v1.0.zip` on the [release page](https://github.com/greasebob/QuartzMIDI/releases) includes both, as does `QuartzMIDI-v1.0.exe`, a single file that keeps them and its settings in `%APPDATA%\QuartzMIDI`. `QuartzMIDI-v1.0-no-addons.zip` leaves them out; add one by extracting its zip into the app's `addons` folder.

- `sheets.zip`: a song as a sheet, copied, saved or opened in the sheet editor.
- `converter.zip`: audio to MIDI, from a file or a link.

## Build

1. Install Visual Studio 2022 or later with the "Desktop development with C++" workload.
2. Open `QuartzMIDI.sln` and build Release x64.
3. Run `build\shell\QuartzMIDI.exe`.

[BUILD.txt](BUILD.txt) covers the add-on key, the converter's setup and later toolsets.

## Requirements

Windows 10 or 11, 64-bit.

## Credits

QuartzMIDI is a fork of [Zephkek/MIDIPlusPlus](https://github.com/Zephkek/MIDIPlusPlus).
It uses [RtMidi](https://github.com/thestk/rtmidi), [Dear ImGui](https://github.com/ocornut/imgui), [nlohmann/json](https://github.com/nlohmann/json), [Lucide](https://lucide.dev) icons and [IBM Plex Sans](third_party/fonts/ibm-plex-sans/README.md).
Prior art: [shizuhaki/miditoqwerty](https://github.com/shizuhaki/miditoqwerty) and [ArijanJ/miditoqwerty](https://github.com/ArijanJ/miditoqwerty).

## License

GPLv3, see [LICENSE](LICENSE).
