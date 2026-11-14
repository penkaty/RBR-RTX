# The F3 menu

**F3** opens RBR-RTX's own menu inside the game window. **Alt+X** is still Remix's menu, and both can be open at once.

## How it behaves

- **The game keeps running behind it.** Stop the car first. The keyboard and mouse go to the menu, also for other plugins, but a steering wheel or pedals still reach the game.
- **The crosshair is the middle of the screen.** Several pages work on whatever surface sits there. Move the camera, not the mouse, to pick something. **Freeze** keeps the last hit so you can move the mouse to the buttons.
- **It costs nothing while closed.** The menu draws only while it is open.
- **It saves to its own file,** `rbr-rtx\config\menu.ini`. It never writes Remix's `user.conf`. The line at the bottom of the window tells you what was saved or why something failed.
- **Another key:** change `menu` in `rbr-rtx\config\keys.ini`. It has to be a plain key, not an Alt combination.

## The pages

| Page | What it is for | Saved |
|---|---|---|
| **Info** | Stage, weather, time of day, ground mode and the full key list | - |
| **Wetness** | Every wet-road slider | With **Save** |
| **Textures** | Tag the surface under the crosshair | At once |
| **Car paint** | Shine and metal of car paint | At once |
| **Atmosphere** | Weather, haze and sun rules for all stages (`atmosphere.ini`) | **Save to file** |
| **Per stage** | The same rules for the stage you are on | **Save to file** |
| **Headlights** | Car lamp settings (`headlights.ini`) | **Save to file** |
| **Lamps** | Street lamps, floodlights, lit windows, free lights | At once |
| **Debug** | Switch single parts of the mod on and off | - |
| **Save** | Write the Wetness values to `menu.ini` | - |

### Wetness
Sliders for puddles, ripples, raindrop rings, darkening and roughness. The top group is what you will normally touch; **Advanced** holds the rain map, shadows, distances and ground detail.

Three values follow the stage by themselves: **Global Wetness**, **Raindrops** and **Rain Intensity**. Move one and it is held until the stage ends or you press **Release locks**. These three are never saved. Everything else is saved when you press **Save** on the Save page.

### Textures
Remix decides how to draw a surface from tags on its texture. This page sets the four tags that matter for stages.

1. Aim the crosshair at the surface. The page shows the shader, the distance and up to two textures, `t0` (base) and `t1` (second layer).
2. Tick **Freeze**, then choose `t0` or `t1`.
3. Press a tag. Pressing it again removes it. **Clear** removes all four.

| Tag | Use it for | Do not use it for |
|---|---|---|
| **Terrain** | Flat ground that faces up: road, grass, gravel | Walls, roofs, steep slopes. They turn black or white, and the page warns you when the surface is not flat enough. |
| **Decal** | Thin layers on a floor: road markings, kerbs, cracks, dirt | Anything without a solid surface under it |
| **Water** | Whole water surfaces | Wet roads and puddles |
| **Ignore** | Things that should not be drawn: flare strips, fake shadows | - |

**Skip foliage** lets the crosshair look through trees. Tags made in Alt+X are not shown here.

### Car paint
The game stores how reflective each part of a car's paint is. RBR-RTX turns that into roughness: **Shiny roughness** for the glossy parts, **Dull roughness** for the matte parts, and **Metallic** for the whole paint. **Enable** switches it off to compare.

### Atmosphere and Per stage
Both pages edit `atmosphere.ini` as input fields. Hover a key to read what it does. **Save to file** writes it, and the change shows within about a second while you drive. **Reload from file** drops what you typed.

Use **Per stage** for one stage whose sun is in the wrong place, and **Atmosphere** only for rules that should hold everywhere (night sun, moon, main menu weather). Clouds, fog and haze themselves are NUMOS weather presets and are tuned in **Alt+X** (developer menu, Sky, Weather), see [How it works](how-it-works.md).

### Headlights
Edits `headlights.ini` the same way: brightness in candela, colour in kelvin, beam angles, when the lamps come on by themselves. A change applies at the next stage start, so restart the stage to see it.

### Lamps
Two tools in one page.

**Lamp heads: glow and light from a part of a texture.** For street lamps, floodlights and anything else the stage draws many times.

1. Aim the crosshair at the lamp head. A yellow box shows the part of the texture you hit.
2. Pick a preset: Streetlamp (sodium), Strong streetlamp, Modern streetlamp (LED), Modern strong streetlamp, Floodlight, Stadium light, Coloured spot, Point light, or **Windows (glow only)**.
3. Press **Make this a lamp** (or **Make emissive** for glow without light).

Every lamp head on the stage drawn with that part of the texture now glows and gets a lamp. In the list below you can change temperature, brightness, cone and softness, choose **at night** or **always**, aim all of them at a point (**Aim here**) or straight **Down**, and move the lamp out of the lamp head with the offset if the mesh blocks it.

With **only the highlighted part** ticked, other objects that share the texture stay dark. If the yellow box is wrong, drag in the texture preview: inside the cyan box to move it, an edge to resize, outside to draw a new one.

**Lit windows:** choose the Windows preset and put the box over the panes of a facade. Each pane gets its own brightness, so a wall shows a believable mix of lit and dark windows.

**Free lights at a point.** For a single light that belongs to nothing: a flare, a doorway, a crowd. **Add at crosshair** puts one on the surface you look at, **Add in front** or **Add cone at camera** puts one where you are. Each light can be moved, aimed, duplicated and deleted. Free lights are kept per stage.

### Debug
Switches for comparing: the ground's second layer (decal, bake, off; applies at the next stage load), a facade fix, and hiding the game's lens flare and sun outline. If a switch fixes something for you, tell us which stage.

## Where things are saved

| File | Holds |
|---|---|
| `rbr-rtx\config\menu.ini` | Wetness values, texture tags, lamps, windows, free lights, car paint |
| `rbr-rtx\config\atmosphere.ini` | Atmosphere and Per stage pages |
| `rbr-rtx\config\headlights.ini` | Headlights page |

The lamps and windows made during development ship in `menu.ini`, so many stages already have them. **Uninstalling removes `rbr-rtx\`: copy `menu.ini` out first if you want to keep your work,** and send it to us if you lit a stage well. It can ship for everyone in the next version.

## Known limits
- A glow sprite set to Ignore in Alt+X can still block the crosshair until the game restarts.
- Lamps you place can look dimmer while the camera points at the ground.
- Unsaved Wetness values are lost if the game closes. Press **Save** when you like what you see.
