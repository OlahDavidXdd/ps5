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
paths and ISO version suffixes. Encrypted retail dumps, UDF-only images, multi-extent and
interleaved ISO files are unsupported. Importing a file does not guarantee game compatibility.

## Install PKG files

Click **Install PKG…**, select a PS5 debug/fake **base** package and a games folder, then click
**Install**. Leave the package passcode blank for the default, or enter its 32-character passcode.
The installed game appears in the library automatically. Cancellation and failed installations
remove partial files; existing games and the original PKG are never overwritten.

Windows packages include the independent [PlayOutPs5Pkg extractor](https://github.com/jeremybris/PlayOut-PS5PKG),
its GPL-3.0 license, notices, and corresponding source in `helpers/pkg`. Other platforms require
a separately built, compatible native PlayOutPs5Pkg executable selected in the install dialog.
Retail-encrypted packages, updates/patches, DLC, PS4 packages, and metadata-only packages are not
supported by this installer. Installing a package does not guarantee that its executables can run.

[Build instructions](docs/building.md) · [Build status](https://github.com/OlahDavidXdd/ps5/actions/workflows/build.yml)

## License

Modified from [KytyPS5](https://github.com/KytyPS5/KytyPS5), based on [Kyty](https://github.com/InoriRus/Kyty).
Licensed under [GPL-2.0-only](LICENSE); original Kyty notices are retained in
[LICENSES/Kyty-MIT.txt](LICENSES/Kyty-MIT.txt). Third-party licenses remain with their components.
