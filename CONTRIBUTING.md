# Contributing

You do not need to write code to help. In order of how much it helps:

## 1. Report problems with data
The three tools are **9** (mark a moment), **8** (record one frame) and **Alt+K** (stage sweep): see
[Troubleshooting](docs/troubleshooting.md) for which to use when. Send the result with stage, car and weather. A sweep
from the exact spot is what lets us fix wrong surfaces, because Remix works with texture hashes.

## 1b. Share what you made in the F3 menu
Street lamps, lit windows and texture tags you make in **F3** are saved in `rbr-rtx\config\menu.ini`. Send the file with
the stage names and they can ship for everyone, see [The F3 menu](docs/f3-menu.md).

## 2. Fix a stage's sun
Match the sun to the stage's baked shadows and send `sun-saved.ini`: see [Fixing the sun](docs/fixing-the-sun.md).

## 3. Test on your hardware
Tell us GPU, driver and resolution along with frame times from **Alt+P**. Tests on cards other than the RTX 2080 Ti
and 3080 are especially welcome, and so are tests on RSF cars and stages we do not mention.

## 4. Code and content
- `src/d3d9-proxy/` is the proxy (C++, x86). `package/` mirrors the `rbr-rtx\` folder that lands in the game.
  `build.ps1` assembles `dist/RBR-RTX/` with the Remix Plus runtime.
- Needs Visual Studio 2022 with the C++ x86 tools:
  ```powershell
  .\build.ps1 -Remix <built runtime folder, from penkaty/dxvk-remix rbr-main>     # -Diagnostics logs from the start, -Pack makes the archives
  .\dist\RBR-RTX\rbr-rtx\install.ps1 -Game <RBR folder>
  ```
  The script also takes the maintainer's F3 lamps and saved suns from a development folder beside the repository; without
  that folder, copy `menu.ini` and `sun-saved.ini` from a release into `dist\RBR-RTX\rbr-rtx\config` yourself.
- Branches: `main` is tested releases, `dev` is work in progress. Open pull requests against **`dev`**.
- Rules that keep the mod installable by everybody: only add files (no game file is changed or replaced), keep it
  removable with `uninstall.cmd`, and never include game assets: it contains none of RBR's files.
- Please test in game before sending code, and say which stage and car you tested on.

## Conduct and security
Be respectful: see the [Code of Conduct](CODE_OF_CONDUCT.md). Found a security problem? Do not open a public issue,
see the [Security Policy](SECURITY.md).

## Licence
RBR-RTX is under the [PolyForm Noncommercial License 1.0.0](LICENSE): free to use, change and share, never to be sold. RTX Remix is NVIDIA's and
keeps its own licence, see [THIRD-PARTY.md](THIRD-PARTY.md).

By sending a contribution you confirm you may do so, and you license it to the project under the project's licence. You also allow the maintainer
to relicense it, including under MIT or Zlib when a general fix goes to dxvk-remix.
