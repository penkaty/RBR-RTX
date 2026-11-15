# Remix settings for strong PCs

RBR-RTX ships tuned for **60 fps at 1080p on an RTX 2080 Ti**: DLSS Performance, Ray Reconstruction, one light bounce
and a medium-quality radiance cache (SHaRC). If your GPU is clearly faster than that, you can trade the headroom for
picture quality.

## How to change settings
- Press **Alt+X**, open **Graphics Settings** (simple) or the developer menu (every option), change values and
  save. Saved settings go to `user.conf` and `rtx.conf` in the RBR folder and stay there, also after updates.
- To return to the shipped defaults, quit the game and delete both files.
- Change one thing at a time and judge it with the same view: stop the car and run the **Alt+K** sweep before and
  after, or watch the frame time in Alt+X.
- A few options jump back at every start because the mod sends them: they are listed in
  `rbr-rtx\config\start-options.ini`. Delete a line there to own that option yourself.
- Do not switch opacity micromaps on or off in the menu while driving: it crashed the GPU in testing.

## What to raise, in the order that usually pays most
| Setting | Shipped | Raise to | Cost / notes |
|---|---|---|---|
| **DLSS quality** (Graphics Settings) | Performance | Balanced, Quality | The biggest picture gain. Performance renders 540p internally at 1080p; cost grows with the internal resolution. |
| **Indirect light cache** (`rtx.sharc.qualityPreset`) | 0 (Medium) | Higher presets | Cleaner, more accurate bounce light |
| **Light bounces** (`rtx.pathMaxBounces`) | 1 | 2 or 3 | More accurate indirect light and reflections; each extra bounce costs frame time |
| **Cloud quality** (Sky settings) | Performance preset | Higher | Sharper clouds; the cloud pass is one of the larger costs after Ray Reconstruction |

## Settings that look like free quality but are not
- **Opacity micromaps** are meant to speed up see-through foliage but gave no gain on RBR's foliage; they stay off.
- **Foliage sway** (**Alt+Comma**) brings swaying trees back, but swaying foliage is captured by Remix every frame
  and costs 0.2 to 2.3 ms on forest stages, plus smudged trails behind the trees.
- **Remix's volumetric fog** doubles the haze NUMOS already draws and costs frame time; it stays off.

## Settings that depend on your taste
Exposure, tone mapping, bloom and the weather presets were tuned by playing. If you find the picture too bright, too
dark or too glossy, change them in the Remix menu and save; if you find a better default, tell us which stage and
weather you tested it on.

If you find a setup that is clearly better on a strong card, please send your `user.conf` and the GPU model: it helps
decide what the next version ships for stronger PCs.
