# An improved Serious Sam Fusion Achievement Enabler
![Github All Releases](https://img.shields.io/github/downloads/flashintv/Sam2017_ModAchievementsEx/total.svg)
![GitHub Issues or Pull Requests](https://img.shields.io/github/issues/flashintv/Sam2017_ModAchievementsEx)
![GitHub commit activity](https://img.shields.io/github/commit-activity/t/flashintv/Sam2017_ModAchievementsEx)

## How to use
Download one of the executables from the latest releases depending on what you want to play, and put it in your game folder *(Serious Sam Fusion 2017\Bin\x64)* alongside other executables:
- **Sam2017_ModAchievementsEx.exe**: vanilla experience with achievements enabled while using in-game cheats
- **Sam2017_Unrestricted_ModAchievementsEx.exe**: modded experience with achievements enabled while using in-game cheats and mods
- **Sam2017_VR_ModAchievementsEx.exe**: vanilla VR experience with achievements enabled while using in-game cheats
- **Sam2017_Unrestricted_VR_ModAchievementsEx.exe**: modded VR experience with achievements enabled while using in-game cheats and mods

## Launcher information
If the game relaunches itself via the *sysStartProcess* function, for example when toggling off and on workshop mods - the launcher will reopen automatically.

The launcher naming scheme must follow `<ORIGINAL EXECUTABLE NAME>_ModAchievementsEx.exe`, if it's not following the format. You won't be able to open the game, although you don't need to worry about renaming the executable if you downloaded it from the Releases, as they are named accordingly.

## Project information
The project works by reverse engineering the internal parts of the engine on how it awards achievements and sets leaderboard stats, and then re-adding the missing code to modded executables and or overriding them to enable them while using in-game cheats.

The hardest task about this project was making it work flawlessly on the public beta of Serious Sam Fusion as the game contains anti-tamper mechanisms, luckily after a couple days of reverse engineering - I believe I was able to track down all of the anti-tamper triggers and stub them out.

Keep in mind this project does not stop the game from flagging your game profile with a cheating flag if you are using in-game cheats during your playthrough, although I believe it shouldn't affect anything. The only thing I think it does is store the data a little differently while having the flag checked. I don't think it even shows that the level stats are cheated on the stats menu.

This project should work with pirated copies and or with DLC unlockers, as it does not touch the Steam binaries. If anybody was wondering that, so yes you can unlock achievements on a Steam emulator or anything like CreamAPI or SmokeAPI which unlocks DLCs while using a legit game copy.

## Credits
- Thank you *learn_more* from the UnKnoWnCheaTs forum for the [signature scanner](https://www.unknowncheats.me/forum/c-and-c/77419-findpattern.html#post650040).
- Thank you *cursey* and all of the contributors for [safetyhook](https://github.com/cursey/safetyhook)
- Thank you *Croteam* for making the Serious Sam franchise