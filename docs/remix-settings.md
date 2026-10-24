# Remix settings for strong PCs

RBR-RTX ships tuned for **60 fps at 1080p on an RTX 2080 Ti**: DLSS Performance, Ray Reconstruction with the
lighter CNN model, one light bounce and a medium-quality radiance cache. If your GPU is clearly faster than that, you
can trade the headroom for picture quality.

## How to change settings
- Press **Alt+X**, open **Graphics Settings** (simple) or the Developer menu (every option), change values and
  press **Save**. Saved settings go to `user.conf` in the RBR folder and override the shipped `rbr-rtx\config\rtx.conf`,
  also after updates.
- To return to the shipped defaults, delete `user.conf` (quit the game first).
- Change one thing at a time and judge it with the same view: stop the car and run **Alt+P** to get frame times.
- Do not switch opacity micromaps on or off in the menu while driving: it crashed the GPU in testing.

## What to raise, in the order that usually pays most
| Setting | Shipped | Raise to | Cost / notes |
|---|---|---|---|
| **DLSS quality** (Graphics Settings) | Performance | Balanced, Quality | The biggest picture gain: ultra performance looked blurry, Performance renders 540p internally at 1080p. Cost grows with the internal resolution. |
| **Ray Reconstruction model** (`rtx.rayreconstruction.model`) | 0 (CNN) | 1 (Transformer) | Sharper, steadier denoising, heavier. A tester's slow 5120x1440 run turned out to be this setting plus the pixel count. |
| **Indirect light quality** (`rtx.neuralRadianceCache.qualityPreset`) | 0 (Medium) | 1 (High), 2 (Ultra) | Cleaner, more accurate bounce light, costs frame time. |
| **Light bounces** (`rtx.pathMaxBounces`) | 1 | 2 or 3 | More accurate indirect light and reflections; each extra bounce costs frame time. |
| **Resolution** | your monitor | | 1440p is 1.8 times and 4K 4 times the pixels of 1080p. |

## Settings that look like free quality but are not
- **Opacity micromaps** are meant to speed up see-through foliage but gave no gain on RBR's foliage; they stay off.
- **Foliage sway** (key **K**, or the sway distance option) brings swaying trees back, but swaying foliage is captured
  by Remix every frame and costs 0.2 to 2.3 ms on forest stages, plus smudged trails behind the trees.
- **Ray Reconstruction off** (denoisers instead) is about 2 to 3 ms faster on older cards if you are short of frame time.

## Settings that depend on your taste
Exposure, tone mapping, bloom and the material defaults were tuned by playing and are saved in `rtx.conf`. If you find
the picture too bright, too dark or too glossy, change them in the Remix menu and **Save**; if you find a better
default, tell us which stage you tested it on.

If you find a setup that is clearly better on a strong card, please send your `user.conf` and the GPU model: it helps
decide what the next version ships for stronger PCs.
