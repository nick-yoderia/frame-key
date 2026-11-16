# Passthrough for native apps on the Steam Frame

Goal: an app running on the Frame draws part of its picture in a key colour
(a video's green screen), and the Frame shows its own camera passthrough
there.

PC VR streams used to be keyed here too. SteamOS 0.5.9 does that itself
(`distort_ps_achromatic_se2_ck`, set on the PC under SteamVR's Camera
settings), so since 0.7 Frame Key leaves the stream's shaders alone.

## How a native app reaches your eyes

1. The app hands the Frame's `vrcompositor` a normal (sRGB) image per eye,
   with an alpha channel.
2. The compositor draws it with two shaders:
   - `distort_ps_achromatic_alphaed` (or `distort_ps_achromatic` for apps
     without alpha) draws the whole view;
   - `fsharp_vr_ps` draws a sharpened circle where the eye tracker says you
     are looking, faded at its edge by alpha.

   It blends the two by alpha.
3. If `camera/forceSceneAlphaBlending` is on, the compositor also draws the
   passthrough camera every frame and blends it in wherever the scene's alpha
   is below 1 (`src = ONE_MINUS_DST_ALPHA, dst = ONE`).

So the Frame can already mix passthrough into an app's picture per pixel; a
video player just never writes alpha below 1 for its green screen.

The layer's trace mode shows which shader draws what: create
`~/.config/frame-keyer/trace` and restart SteamVR; every 5 s it logs how often
each pipeline was bound to `layer.log`. Off-head the cameras and foveation
don't run, so wear the headset while tracing.

## The keyer (`headset/`)

A Vulkan layer that loads only into `vrcompositor` and swaps those three
shaders for patched copies (`gen_key.py`). The camera image never leaves the
compositor; we only change the alpha the app's picture is drawn with.

- The keyed copies compare the colour in sRGB (the app's pixels are linear)
  and multiply the app's alpha by the key's. Replacing the app's alpha with
  the key's alone breaks the circle: you get a sharp circle where you look and
  garbage around it.
- Alpha ramps linearly from 0 at the tolerance to 1 at tolerance + softness.
  The distance is CbCr divided by brightness (max channel + 0.1), so all
  shades of the key's colour match: with plain CbCr the Green preset missed
  darker green screens (a (0, 142, 0) screen keyed 0%), and a dull eyedropper
  pick of a filmed screen sat so close to grey that people, white and black
  keyed too. Pixels darker than 0.09..0.15 (max channel) stay solid, since
  their colour is mostly compression noise.
- Despill: near the key, a pixel loses the key's tint, so the half-key fringe
  that video compression smears around shapes turns neutral. It reaches only
  a little past the key's edge (tolerance + softness), so colours merely near
  the key, like orange under a red key, keep their tint.
- With `forceSceneAlphaBlending` on, the camera is drawn behind opaque native
  apps too, so no change is needed in the app. It also disables SteamVR's
  "camera at room edge" passthrough while it's on.
- Tied to SteamVR's shaders (tables for SteamOS 0.5.6 and 0.5.7 to 0.5.9).
  `gen_key.py` refuses to patch if the layout changed, and then nothing is
  keyed. Each keyed copy is stored with the stock shader it was made from; the
  layer only uses it while SteamVR's shader is still that one, and falls back
  to the stock shader if the patched one is rejected.

The Frame Key dashboard panel (`panel.cpp`) drives it; underneath it runs
`keyer.sh chroma R G B tolerance softness spill` or `keyer.sh off`.

The key colour and sliders aren't baked into the patched shaders: they read
them from push constants (small values handed to a shader with each draw).
The layer adds a push constant range to the compositor's pipeline layouts
(none of its own shaders use push constants) and pushes the settings from
`~/.local/share/frame-keyer/params` whenever it binds a keyed shader. So only
turning keying on or off reloads shaders (`vrcmd --reload-shaders`, which
makes the compositor rebuild every pipeline); a settings change rewrites that
file and shows on the next frame, and the panel applies sliders while you
drag. Thirteen reloads in 40 s during a 4K video once stalled the compositor
until the system killed it.

The eyedropper reads a 9x9 patch at the centre of SteamVR's headset-view
mirror (`/dev/video99`) with keying paused, so it sees the app's picture and
never the cameras.

`headset/xrkeytest.cpp` is a small native OpenXR app (key colour with a grey
square in the middle) to test this.

Credit: this builds on [steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key)
by spyjacob (MIT), first posted on OpenVR issue #1926. The panel's design and
parts of its code, and the idea of patching the compositor's shaders from a
Vulkan layer in vrcompositor, come from there.

## Limits of a chroma key

- Any pixel near the key colour also turns into passthrough.
- Video compression smears colour at edges, so edges need some softness.
- A black background can't be colour-keyed.
