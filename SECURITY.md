# Security Policy

RBR-RTX adds a `d3d9.dll` and an `rbr-rtx\` folder to Richard Burns Rally and runs inside the game. It sends nothing
over the network: stage reports and logs stay on your PC until you choose to share them.

## Supported versions
Only the latest release on the [releases page](https://github.com/penkaty/RBR-RTX/releases) gets fixes.

## Reporting a vulnerability
Please do **not** open a public issue for a security problem. Report it privately through GitHub instead:
**Security** tab of this repository, then **Report a vulnerability**. Include the version, what you found, and how
to reproduce it.

You will get an answer within a week. Once fixed, the fix ships in the next release and the report is credited
unless you ask otherwise.

## Out of scope
- RTX Remix itself (`d3d9.dll` loader, bridge and runtime from NVIDIA): report those to
  [NVIDIA's RTX Remix project](https://github.com/NVIDIAGameWorks/rtx-remix).
- The game, RSF or other plugins.
- Downloads of RBR-RTX from anywhere other than this repository's releases. Only use the official releases.
