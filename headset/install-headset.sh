#!/bin/sh
# Install the headset side on the Steam Frame (run on the Frame, from the
# unpacked release's headset/ folder, or let Install.cmd on the PC do it):
#   - the compositor layer that keys the Steam Link stream
#   - the "Frame Key" panel in the SteamVR dashboard, started with SteamVR
# Everything goes under your home folder; nothing in /usr or /opt is touched.
#   sh install-headset.sh            install and restart SteamVR (closes running VR apps)
#   NO_RESTART=1 sh install-headset.sh
set -eu
cd "$(dirname "$0")"
D=$HOME/.local/share/frame-keyer
CONF=$HOME/.config/frame-keyer
MANIFEST=$HOME/.local/share/vulkan/implicit_layer.d/frame-keyer.json
UNIT=$HOME/.config/systemd/user/frame-keyer-panel.service
SHADER=/opt/steamvr/resources/shaders/vulkan/distort_ps_achromatic_se2.spv

say() { printf '%s\n' "$*"; }
[ "$(uname -m)" = aarch64 ] || { say "This is for the Steam Frame (aarch64); this machine is $(uname -m)."; exit 1; }
[ -f "$SHADER" ] || { say "SteamVR's stream shader isn't where expected ($SHADER)."; exit 1; }
for t in spirv-dis spirv-as spirv-val python3; do
	command -v $t >/dev/null || { say "Missing $t (SteamOS normally has it)."; exit 1; }
done
command -v ffmpeg >/dev/null || say "Note: no ffmpeg, so the panel's eyedropper won't work."

# This SteamVR's shader must match what the keyer knows how to patch.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
spirv-dis "$SHADER" -o "$T/se2.spvasm"
if ! python3 gen_key.py "$T/se2.spvasm" "$T/test.spv" chroma 1 0 1 >/dev/null 2>"$T/err"; then
	say "This SteamVR version's stream shader changed; the keyer can't patch it:"
	cat "$T/err"
	say "Look for a newer Frame Key release; your SteamVR is unchanged."
	exit 1
fi
# Native apps' shader: keying them is a bonus, so a mismatch only warns.
spirv-dis "${SHADER%_se2.spv}_alphaed.spv" -o "$T/alphaed.spvasm" 2>/dev/null &&
	python3 gen_key.py "$T/alphaed.spvasm" "$T/test2.spv" chroma 0 1 0 >/dev/null 2>&1 ||
	say "Note: this SteamVR's shader for native apps changed, so only PC streams get keyed."

systemctl --user stop frame-keyer-panel.service 2>/dev/null || true
mkdir -p "$D/bin" "$CONF" "$(dirname "$MANIFEST")" "$(dirname "$UNIT")"
install -m 755 bin/frame-keyer-panel bin/vrsetting "$D/bin/"
install -m 644 bin/libopenvr_api.so "$D/bin/"
install -m 755 libVkLayer_frame_keyer.so keyer.sh "$D/"
install -m 644 gen_key.py "$D/"
cat > "$MANIFEST" <<EOF
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_FRAME_KEYER",
    "type": "GLOBAL",
    "library_path": "$D/libVkLayer_frame_keyer.so",
    "api_version": "1.4.309",
    "implementation_version": "1",
    "description": "Chroma-key passthrough for Steam Link streams (vrcompositor only)",
    "functions": { "vkNegotiateLoaderLayerInterfaceVersion": "keyer_NegotiateLoaderLayerInterfaceVersion" },
    "disable_environment": { "DISABLE_FRAME_KEYER": "1" }
  }
}
EOF
touch "$CONF/enable"
sed "s#@PANEL@#$D/bin/frame-keyer-panel#" frame-keyer-panel.service > "$UNIT"
systemctl --user daemon-reload
systemctl --user enable frame-keyer-panel.service >/dev/null
# "Frame Key" in the Frame's app list (+): starts the panel after Quit, or opens it.
ICONS=$HOME/.local/share/icons/hicolor/128x128/apps
mkdir -p "$HOME/.local/share/applications" "$ICONS"
install -m 644 frame-keyer.png "$ICONS/"
cat > "$HOME/.local/share/applications/frame-keyer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Frame Key
Comment=Passthrough keying for PC VR streams; opens its SteamVR dashboard panel
Exec=$D/bin/frame-keyer-panel --show
Icon=frame-keyer
Categories=Utility;
Terminal=false
EOF
say "Installed to $D."

if ! systemctl --user cat steamvr.service >/dev/null 2>&1; then
	say "Restart SteamVR to finish (no steamvr user service found to restart it from here)."
elif [ -n "${NO_RESTART:-}" ]; then
	# the panel was stopped above; bring it back next to a running SteamVR
	systemctl --user -q is-active steamvr.service && systemctl --user start frame-keyer-panel.service
	say "Restart SteamVR to finish (systemctl --user restart steamvr)."
else
	say "Restarting SteamVR (about 15 s)..."
	systemctl --user restart steamvr
	say "Done. Open the SteamVR dashboard and look for Frame Key."
fi
