# Help wanted: sky, sun and weather

So far I have focused on much needed performance fixes and on cutting the unused vertex calls and draws. Still ahead
is an ungodly amount of work that can only be done in a reasonable time with the help of people willing to test it.

RBR does not light its scenery in real time: the light and shadows are baked into the stage once per time of day.
RBR-RTX gives Remix a real sun, so the path-traced shadows (trees, cars, walls) have to land where the baked ones are.
When the two disagree you get double shadows, light from the wrong side and lit slopes that should be in shade.
Nothing in the game files tells us when they disagree (of 224 stages with several times of day, 169 use a different sun
per time), so **only a player looking at the stage can tell**. From easiest to hardest:

## 1. Sky textures that block the sun (easiest)
Some stages use a skybox that literally blocks the sun. Pick that texture with the Remix dev tools (**Alt+X**) and give
it the *sky texture* flag, then **Save**. Send me the `rtx.skyBoxTextures` line from `user.conf` in the RBR folder, with
the stage name. RabocsiRing 2021 was fixed this way.

## 2. Sun position
Stages differ in how they bake their light. RBR-RTX reads the sun direction left in the stage files and places the
Remix sun to match. Some stages lack it entirely or use a straight-up 90 degree sun. For those the sun position is
automated from the chosen weather, an approximate time of day and the stage's location. This is where manual input is
needed: every stage and time of day you set right is fixed for all players in the next release.

**How, to set the sun position in under 20 seconds:**
1. Be on the stage, in the weather and time of day you want to fix. Stopping the car helps.
2. Press **`/`**: the debug sun starts at the game's sun.
3. Hold **`[`** / **`]`** to turn it around and **`;`** / **`'`** to lower / raise it, until the light matches the
   baked shadows (long shadows from trees and rocks are the easiest to judge).
4. Press **`\`**. A Windows beep means saved. That sun is used for this stage and time of day from now on, also after
   you press **`/`** again and next time you drive it.
5. The suns are in `rbr-rtx\config\sun-saved.ini`. Send the file ([GitHub issues](https://github.com/penkaty/RBR-RTX/issues))
   with the stage names if you remember them. Uninstalling removes `rbr-rtx\`, so copy it out first.
6. To go back to the stage's own sun, delete that stage's lines from `sun-saved.ini`.

**Stages already fixed:** Carvalho de Rei 2008 reverse (morning), Santa Lucia (morning, noon), Olocau Gatova (morning),
Gestel I (morning), La Atalaya II (noon), Whaanga Coast Sprint (morning). The values live in
`rbr-rtx\config\atmosphere.ini` under `[stage.<id>]`; your `sun-saved.ini` is read before it.

## 3. Sun intensity and the weather look (hardest)
Even when the sun position fits the bake and the theme of the stage, its intensity has to be regulated properly. There
are two ways: dim and brighten the sun, which works today with **Alt+N** / **Alt+M** (10 % per press, for the game sun
and the debug sun; the log in `rbr-rtx\logs\rbrrtx_comp.log` shows the value to keep), and enlarge it so shadows get
softer, which is not in yet. Rosciszow-Walim and its reverse are baked with deliberately soft shadows and already get a
weaker sun.

The goal is a full volumetric atmospheric weather system that matches the original raster look (heavy fog, distant
haze, overcast, clear day). In many cases the weather name alone will not give enough information, so manual input will
be needed here too.
