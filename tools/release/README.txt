Frame Key
=========

See-through spots in apps running on a Steam Frame. Anything an app draws in
the key colour shows the Frame's passthrough camera instead, for example the
green screen behind a 360 or 3D video (set the key to Green).

PC VR streams are not touched: since SteamOS 0.5.9, SteamVR keys Steam Link
streams itself (on the PC: SteamVR settings > Camera, mode Key Content).
Frame Key releases before 0.7 keyed the stream and installed an OpenXR layer
on the PC; Install.cmd removes that.

Before you install (once)
-------------------------
  On the Frame: Steam Settings > System > Enable Developer Mode. Then in
  the left menu scroll down to the Developer section and, near the bottom of
  that page, click Set User Password.
  The PC and the Frame must be on the same network. Note the Frame's IP
  address (its Wi-Fi settings, or your router's device list).

Install
-------
  Extract this zip to a folder, then double-click Install.cmd. It installs
  Frame Key on the headset over SSH: it asks for the Frame's address and,
  the first time only, its password. SteamVR on the Frame restarts at the
  end. Nothing is installed on the PC.
  To update, run the new release's Install.cmd; settings are kept.

Use
---
  On the Frame: SteamVR dashboard > Frame Key (magenta ring icon).
  Turn it On, pick the key colour (presets, hue bar, or Eyedropper: look at
  the colour, it is picked at the dot after a 3 s countdown) and adjust
  Tolerance / Edge softness / Spill removal (each explained on the panel).
  Quit turns keying off and closes the panel; it starts again with SteamVR
  or from the Frame's app list (+ > Frame Key).

  Green-screen video: Green suits bright computer-made green; for a filmed
  green screen use the Eyedropper on it. Details in README.md on GitHub.

  While keying is On, SteamVR's "camera at room edge" passthrough doesn't
  show, and native games' colours close to the key turn see-through too.
  Turn keying Off when you don't need it.

Remove
------
  Uninstall.cmd

Unofficial: this patches the Frame's compositor shaders in memory (nothing in
/opt is changed). A SteamVR update can change those shaders; the installer
checks first and refuses if it can't patch the new ones.

Credits: builds on steam-frame-chroma-key by spyjacob (MIT), which the
panel's design and parts of its code come from:
  https://github.com/spyjacob/steam-frame-chroma-key
