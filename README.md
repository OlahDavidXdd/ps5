# PS5 Emulator

[**Download the latest build**](https://github.com/OlahDavidXdd/ps5/releases/latest) · [All releases](https://github.com/OlahDavidXdd/ps5/releases)

Open a release and expand **Assets** to download the package for your computer:

- **Windows:** download `*-Windows-x64.zip`, extract it, and open `kyty_launcher.exe`.
- **Linux:** download `*-Linux-x86_64.tar.gz`, extract it, and run `kyty_launcher`.
- **macOS:** download `*-macOS-x86_64.zip`, extract it, and open `KytyPS5.app` (experimental; Apple Silicon requires Rosetta 2).

Packages appear after their platform builds and tests pass; source archives are not ready-to-run apps.

## Load games

Add a game folder in the launcher's global settings, select a detected game, and run it.
The scanner finds extracted games and `.zar` or `.iso` images containing `eboot.bin` at the root.
No PlayStation account is required; optional PSN simulation is local and provides no online services.

This version adds read-only ISO 9660 loading without extraction, including ASCII case-insensitive
paths and ISO version suffixes. Encrypted dumps, UDF-only images, PKG packages, multi-extent and
interleaved ISO files are unsupported. Importing a file does not guarantee game compatibility.

[Build instructions](docs/building.md) · [Build status](https://github.com/OlahDavidXdd/ps5/actions/workflows/build.yml)

## License

Modified from [KytyPS5](https://github.com/KytyPS5/KytyPS5), based on [Kyty](https://github.com/InoriRus/Kyty).
Licensed under [GPL-2.0-only](LICENSE); original Kyty notices are retained in
[LICENSES/Kyty-MIT.txt](LICENSES/Kyty-MIT.txt). Third-party licenses remain with their components.
