DIAssetBrowser - Diablo Immortal asset browser
==============================================

A native browser for the PC (Battle.net) Diablo Immortal install: browse all
~677,000 assets, preview textures (BC7 decode, channel isolation, mip picker,
PNG export), view models in a live 3D viewport, assemble outfits in the
Wardrobe, export rigged .glb with the real bone hierarchy and animations, and
bulk-extract any filtered set as raw original bytes or decoded PNGs.

Not affiliated with or endorsed by Blizzard Entertainment or NetEase.


Getting started
---------------
1. Unzip this folder anywhere (a USB stick works) and run DIAssetBrowser.exe.
2. On first run, use File > Set game folder... and pick your Diablo Immortal
   install (or its Package\MPK folder).
3. The status bar shows the asset count once the index is built (a few seconds
   the first time; cached afterwards). Every tab has a search box and a
   Filters funnel.


Portable by design
------------------
Everything the tool writes lives in the "data" folder beside the exe:

    data\DIAssetBrowser.ini      settings
    data\DIAssetBrowser.log      runtime log
    data\index_cache\ ...        asset index + thumbnail caches
    data\di_item_names.csv       optional: real cosmetic names (see below)

No registry keys. No %AppData%. Delete the folder and nothing is left behind.


What it needs
-------------
* Windows 10 or 11, 64-bit.
* A GPU with OpenGL 3.3 (anything from roughly 2010 onward). Only the 3D
  viewport needs it - textures, browsing and bulk extraction work without.
* The Visual C++ runtime, WHICH IS INCLUDED in this zip (vcruntime140.dll,
  vcruntime140_1.dll, msvcp140.dll sit beside the exe). You do not need to
  install anything.

If the app will not start with a message about a missing DLL, the zip was
probably extracted partially - extract it again, keeping the whole folder.
Once it is running, Settings > Maintenance > Prerequisites checks both of the
above and can download Microsoft's official runtime installer if needed.

That prerequisites page is the ONLY thing this app ever downloads. It makes no
other network request and works entirely offline.


Real cosmetic names (optional)
------------------------------
DI's assets are named structurally ("f_barbarian_yifu_t07_004"). To see real
in-game set names, use File > Names > Export template, fill in the name column
of the CSV, save it as data\di_item_names.csv, and use Names > Reload.


Reporting problems
------------------
Attach data\DIAssetBrowser.log (or Help > Export log) and say which asset you
clicked. Settings > Information documents how the tool reads the format.
