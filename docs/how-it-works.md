# Passthrough for PCVR on the Steam Frame

Goal: a PC game or overlay (Reality Mixer, a sim) marks part of its image as
"see-through", and the Frame shows its own camera passthrough there while
streaming over Steam Link.

## How the stream reaches your eyes

1. On the PC, SteamVR's compositor merges the game and all overlays into one
   image per eye.
2. `vrlink` encodes that image as H.264/HEVC video (NV12: brightness plus
   colour, **no alpha channel**) and sends it to the headset.
3. On the Frame, `vrlink` decodes the video into GPU buffers and hands them to
   the Frame's `vrcompositor`.
4. The Frame's compositor draws the video with the shader
   `distort_ps_achromatic_se2`, which writes alpha = 1 everywhere.
5. If `camera/forceSceneAlphaBlending` is on, the compositor also draws the
   passthrough camera every frame and blends it in wherever the scene's alpha
   is below 1 (`src = ONE_MINUS_DST_ALPHA, dst = ONE`).

So the Frame can already mix passthrough into a stream per pixel. Two things
are missing: the stream carries no alpha, and the shader in step 4 hardcodes
alpha to 1.

## What Reality Mixer does

Reality Mixer is a PC SteamVR overlay app. Its boxes are world-locked
overlays. With Quest + Virtual Desktop it paints them a solid colour
(`RenderSolidColorIsoCamPixels`, default `#0047bb`), and Virtual Desktop's
headset app replaces that colour with passthrough: a chroma key on the
headset side. (On Quest it can't do camera chroma keying itself, because Meta
doesn't give apps camera pixels.)

## The headset side (`headset/`)

A Vulkan layer that loads only into `vrcompositor` and swaps
`distort_ps_achromatic_se2` for a patched copy (`gen_key.py`). The patch
computes alpha from the stream colour: alpha ramps linearly from 0 at the
threshold to 1 at threshold + softness (CbCr distance from the key), so the
compositor's own passthrough shows where the stream is the key colour. The
camera image never leaves the compositor; we only change the alpha the stream
writes.

- With foveated streaming each eye is a sharp inset plus a low-res periphery,
  blended in the same shader. The key is tested on that blend, so both parts
  key alike (keying only the inset left pink boxes in the periphery).
- Despill: near the key, a pixel loses the key's tint, so the half-key fringe
  that video compression smears around shapes turns neutral instead of pink.
  It reaches only a little past the key's edge (tolerance + softness), so
  colours merely near the key, like orange under a red key, keep their tint.
- `rect` mode cuts a fixed rectangle per eye (it doesn't line up between eyes;
  test only). `keyer.sh off` restores the stock shader live.
- Tied to this SteamVR build's shader (SteamOS 0.5.6). `gen_key.py` refuses
  to patch if the layout changed; the layer falls back to the stock shader if
  the patched one is rejected.

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
until the system killed it. The
eyedropper reads a 9x9 patch at the centre of SteamVR's headset-view mirror
(`/dev/video99`) with keying paused, so it only ever sees the PC stream.

Credit: this builds on [steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key)
by spyjacob (MIT), first posted on OpenVR issue #1926. The panel's design and
parts of its code, and the idea of patching the stream shader from a Vulkan
layer in vrcompositor, come from there.

## Native apps on the Frame

Apps running on the Frame itself reach the compositor as normal (sRGB) images
with an alpha channel, not as video. The layer's trace mode (create
`~/.config/frame-keyer/trace`, restart SteamVR; it logs which shader each draw
uses, every 5 s, to `layer.log`) shows two shaders per native frame:

- `distort_ps_achromatic_alphaed` (or `distort_ps_achromatic` for apps
  without alpha) draws the whole view.
- `fsharp_vr_ps` draws a sharpened circle where the eye tracker says you are
  looking, faded at its edge by alpha.

The compositor blends the two by alpha. The keyed copies compare the colour
in sRGB (the app's pixels are linear), and multiply the app's alpha by the
key's. Unlike the stream, they compare CbCr divided by brightness (max
channel + 0.1), so all shades of the key's colour match: with plain CbCr the
Green preset missed darker green screens (a (0, 142, 0) screen keyed 0%),
and a dull eyedropper pick of a filmed screen sat so close to grey that
people, white and black keyed too. Pixels darker than 0.09..0.15 (max channel)
stay solid, since their colour is mostly compression noise. The stream keeps
plain CbCr because the PC layer writes partial alpha onto that ramp. Replacing the app's alpha with the key's alone breaks the circle: you
get a sharp circle where you look and garbage around it. With
`forceSceneAlphaBlending` on, the camera is drawn behind opaque native apps
too, so no change is needed in the app.

`headset/xrkeytest.cpp` is a small native OpenXR app (key colour with a grey
square in the middle) to test this.

## PC side (`pc/`)

- **Overlay apps (Reality Mixer and similar):** paint the box in the key
  colour. `keybox.exe [front|hand] [RRGGBB]` is a stand-in for testing.
- **OpenXR apps that ask for `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`:**
  `alphakey/` is an OpenXR API layer. It adds ALPHA_BLEND to the blend modes
  SteamVR reports; when an app uses it, it rewrites each scene image just
  before the app hands it back:
  - transparent pixels become the key colour,
  - opaque pixels close to the key are pushed `guard` away, so app content
    can't key by accident,
  - partly transparent pixels are written onto the keyer's ramp (chroma
    toward the content's hue, at a distance proportional to alpha), so the
    Frame decodes roughly the same alpha (0.5 comes back as about 0.6),

  and submits the frame as OPAQUE. D3D11 apps only so far.
  `alphakey.ini`'s `ramp` must match the keyer's threshold and
  threshold + softness.
- **OpenVR scene apps (most sims):** they have no way to say "transparent", so
  only overlays or the key colour in the game's own image work.

The key colour and ramp come from the headset over the LAN: once an app uses
ALPHA_BLEND, the layer broadcasts `FPPQ1` (UDP, port 47911) every 2 s on each
network and the headset panels answer with their settings plus `toyou=1` if
the asking PC is the peer of their `vrlink` sockets. The layer follows that
headset, else the one pinned in `alphakey.ini` (`headset = <id>`), else the only
streaming one, else the only one. `alphakey.ini` also has `enabled = 0|1`, and
its `key`/`ramp` are used until a headset answers.

## Tested (2026-10-26, SteamOS 0.5.6, Steam Link at auto foveated size)

- Magenta overlay box (fixed and on a controller): passthrough, no pink edge
  in the periphery after the foveation fix, no black boxes.
- `xralphatest.exe` through `alphakey`: room everywhere, opaque orange quad and
  blue band, fpsVR overlay drawn over the room.
- SteamVR Home's aurora has magenta rays that also key (expected with a
  chroma key; Virtual Desktop has the same trade-off and a "similarity" slider).
- `camera/forceSceneAlphaBlending` disables SteamVR's "camera at room edge"
  passthrough while it's on.

## Limits of a chroma key

- Any pixel near the key colour in an overlay or OpenVR game also turns into
  passthrough. Magenta (`#ff00ff`) is rare in games; the default Reality Mixer
  blue is not (sky). OpenXR apps through `alphakey` are protected by `guard`.
- Video compression smears colour at edges, so edges need some softness.
- Partial alpha is approximate (8-bit video, compression noise).

A real alpha channel in the stream would remove all three, but that needs
Valve: `vrlink` on both ends would have to carry it.
