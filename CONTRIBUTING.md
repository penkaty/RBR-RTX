# Contributing

You do not need to write code to help. In order of how much it helps:

## 1. Report problems with data
Run the sweep and send it with stage, car and weather: see [Troubleshooting](docs/troubleshooting.md). Stage reports (the
zips in `rbr-rtx\logs\reports`) with a **9** mark at the moment of a problem are just as useful. Reports with a
sweep from the exact spot are what let us fix wrong surfaces, because Remix works with texture hashes.

## 2. Fix a stage's sun
Match the sun to the stage's baked shadows and send `sun-saved.ini`: see [Fixing the sun](docs/fixing-the-sun.md).

## 3. Test on your hardware
Tell us GPU, driver and resolution along with frame times from **Alt+P**. Tests on cards other than the RTX 2080 Ti
and 3080 are especially welcome, and so are tests on RSF cars and stages we do not mention.

## 4. Code and content
- `src/d3d9-proxy/` is the proxy (C++, x86). `package/` mirrors the `rbr-rtx\` folder that lands in the game.
  `build.ps1` assembles `dist/RBR-RTX/` with NVIDIA's Remix runtime.
- Needs Visual Studio 2022 with the C++ x86 tools:
  ```powershell
  .\build.ps1 -Remix <extracted RTX Remix 1.5.2 release>     # add -Diagnostics for tester builds
  .\dist\RBR-RTX\rbr-rtx\install.ps1 -Game <RBR folder>
  ```
- Branches: `main` is tested releases, `dev` is work in progress. Open pull requests against **`dev`**.
- Rules that keep the mod installable by everybody: only add files (no game file is changed or replaced), keep it
  removable with `uninstall.cmd`, and never include game assets: it contains none of RBR's files.
- Please test in game before sending code, and say which stage and car you tested on.
- Material and PBR content belongs in [RBR-RTX-base-mod](https://github.com/penkaty/RBR-RTX-base-mod).

## Conduct and security
Be respectful: see the [Code of Conduct](CODE_OF_CONDUCT.md). Found a security problem? Do not open a public issue,
see the [Security Policy](SECURITY.md).

## Licence
MIT, see [LICENSE](LICENSE). RTX Remix is NVIDIA's and keeps its own licence.
