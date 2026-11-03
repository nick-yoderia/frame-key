Frame Key
=========

See-through spots in PC VR games streamed to a Steam Frame over Steam Link,
and in native apps on the Frame. Anything drawn in the key colour (magenta
by default) shows the Frame's passthrough camera instead, for example
Reality Mixer's "solid color" boxes, or the green screen behind a 360 video
(set the key to Green). OpenXR apps on the PC that ask for transparency
(ALPHA_BLEND) work too.

Using Reality Mixer with SteamVR 2.19 or later? Also install
realitymixer-foveation-fix, or its boxes fade out away from where you look:
  https://github.com/nick-yoderia/realitymixer-foveation-fix

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
  the PC side (no admin, "Frame Key" in the Start menu), then the headset
  side over SSH: it asks for the Frame's address and, the first time only,
  its password. SteamVR on the Frame restarts at the end.
  To update, run the new release's Install.cmd; settings are kept.

Use
---
  On the Frame: SteamVR dashboard > Frame Key (magenta ring icon).
  Turn it On, pick the key colour (presets, hue bar, or Eyedropper: look at
  the colour, it is picked at the dot after a 3 s countdown) and adjust
  Tolerance / Edge softness / Spill removal (each explained on the panel).
  Quit turns keying off and closes the panel; it starts again with SteamVR
  or from the Frame's app list (+ > Frame Key).
  The PC follows the headset on its own over your network (no SSH needed
  after installing). Start menu > Frame Key shows which headset this
  PC follows, switches the PC layer on/off and shows a test box.
  Paint what should be see-through in the key colour.

  Reality Mixer: Camera Setup > Use Solid Color on, then set the panel to
  the same colour (use the Eyedropper on one of its boxes). Its own
  ChromaKey setting isn't used. A box needs an activation option to show;
  for an always-on window pick activation by distance with minimum and
  maximum opacity both at 100%. Boxes that fade with distance only fade the
  passthrough over the first part of the fade. Details in README.md.

  While keying is On, SteamVR's "camera at room edge" passthrough doesn't
  show, and native games' colours close to the key turn see-through too.
  Turn keying Off when you don't need it.

Remove
------
  Uninstall.cmd

Unofficial: this patches the Frame's compositor shader in memory (nothing in
/opt is changed). A SteamVR update can change that shader; the headset
installer checks first and refuses if it can't patch the new one.

Credits: builds on steam-frame-chroma-key by spyjacob (MIT), which the
panel's design and parts of its code come from:
  https://github.com/spyjacob/steam-frame-chroma-key
