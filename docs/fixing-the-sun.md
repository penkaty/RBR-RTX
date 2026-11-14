# Fixing the sun

RBR does not light its scenery in real time: the light and shadows are baked into the stage once per time of day.
RBR-RTX gives the stage a real sun, so the path-traced shadows (trees, cars, walls) have to land where the baked ones are.
When the two disagree you get double shadows, light from the wrong side and lit slopes that should be in shade.

Nothing in the game files tells us when they disagree: of 224 stages with several times of day, 169 use a different sun
per time. **Only a player looking at the stage can tell.** Every stage you set right can ship for everyone in the next version.

## Sun position
RBR-RTX reads the sun direction left in the stage files. Some stages lack it or use a straight-up sun; for those the
position is worked out from the weather, the time of day and the stage's country, and that is where it can be wrong.

**Setting it takes under 20 seconds:**
1. Be on the stage, in the weather and time of day you want to fix. Stopping the car helps.
2. Press **`/`**: the debug sun starts at the game's sun.
3. Hold **`[`** / **`]`** to turn it around and **`;`** / **`'`** to lower / raise it, until the light matches the
   baked shadows. Long shadows from trees and rocks are the easiest to judge.
4. Press **`\`**. A Windows beep means saved. That sun is used for this stage and time of day from now on.
5. The suns are in `rbr-rtx\config\sun-saved.ini`. Send the file ([GitHub issues](https://github.com/penkaty/RBR-RTX/issues))
   with the stage names if you remember them. Uninstalling or updating replaces it, so copy it out first.
6. To go back to the stage's own sun, delete that stage's lines from `sun-saved.ini`.

A sun you know in numbers can also be typed in: **F3**, page **Per stage**, key `sun_dir.<Time>` (direction and height
in degrees) for the stage you are on.

## Sun strength
How bright and how soft the sun is comes from the sky: NUMOS dims and softens it through clouds, haze and fog. If a
stage is too harsh or too dark in one weather, that weather's preset is the place to change it, see
[Particles, rain, snow and weather](particles-and-weather.md).

## A sky texture that blocks the sun
On a few stages a leftover sky mesh shades the whole stage. Open **Alt+X**, pick that texture and give it the
*sky texture* flag, then save. Tell us the stage and send the `rtx.skyBoxTextures` line from `rtx.conf` in the RBR folder.
