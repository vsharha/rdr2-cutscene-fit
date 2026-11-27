CutsceneFit shows Red Dead Redemption 2 cutscenes at their intended framing on
any screen wider than 16:9, as large as the screen allows and never cropped. It
narrows the camera's field of view so the 2.35:1 frame fills as much of the
screen as it can, and fills what is left with the game's own bars.

Tested on `RDR2.exe` 1.0.1491.50 at 3440x1440, 3440x1200 and 2880x1440.

## Changes in 1.0.3

- Menus that the game boxes to 16:9, such as the general store's outfit
  preview, are no longer zoomed, and keep the game's own side bars. The zoom
  had cut off Arthur's head and feet there.
- The game's videos keep the game's own side bars instead of the plugin's
  narrow ones, which had shown streaks beside the video.
- A cutscene that follows a video is now zoomed from the moment the video
  ends. Before, the zoom waited for a camera cut, and some of those cutscenes
  have none.

## Requirements

- Red Dead Redemption 2 on PC, story mode only, version 1.0.1491.50. On other
  versions the plugin finds its code by byte patterns and stays inactive if any
  of them does not match exactly once.
- An ASI loader, such as the one in Script Hook RDR2 or Lenny's Mod Loader from
  rdr2mods.com. Script Hook itself is not needed.

The plugin does not detect Red Dead Online. Remove it before playing Online.

## Installing

Copy `CutsceneFit.asi` into the game folder, next to `RDR2.exe`. Copy
`CutsceneFit.ini` there too to change the settings. On Linux under Proton, add
`WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's launch options so Wine
loads the ASI loader's `dinput8.dll`.

The End key shows or hides the bars, on any screen.
