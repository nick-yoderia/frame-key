#!/bin/sh
# Install the headset side on the Steam Frame (run on the Frame, from the
# unpacked release's headset/ folder, or let Install.cmd on the PC do it):
#   - the compositor layer that keys native apps (PC streams are left alone)
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
SHADERS=/opt/steamvr/resources/shaders/vulkan

say() { printf '%s\n' "$*"; }
[ "$(uname -m)" = aarch64 ] || { say "This is for the Steam Frame (aarch64); this machine is $(uname -m)."; exit 1; }
[ -d "$SHADERS" ] || { say "SteamVR's shaders aren't where expected ($SHADERS)."; exit 1; }
for t in spirv-dis spirv-as spirv-val python3; do
	command -v $t >/dev/null || { say "Missing $t (SteamOS normally has it)."; exit 1; }
done
command -v ffmpeg >/dev/null || say "Note: no ffmpeg, so the panel's eyedropper won't work."

# This SteamVR's shaders for native apps must match what the keyer knows how to patch.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
for f in distort_ps_achromatic_alphaed distort_ps_achromatic fsharp_vr_ps; do
	if ! spirv-dis "$SHADERS/$f.spv" -o "$T/$f.spvasm" 2>"$T/err" ||
		! python3 gen_key.py "$T/$f.spvasm" "$T/$f.spv" chroma >/dev/null 2>>"$T/err"; then
		say "This SteamVR version's $f shader changed; the keyer can't patch it:"
		cat "$T/err"
		say "Look for a newer Frame Key release; your SteamVR is unchanged."
		exit 1
	fi
done

systemctl --user stop frame-keyer-panel.service 2>/dev/null || true
# Releases before 0.7 also keyed the Steam Link stream; drop that keyed copy.
rm -f "$D/se2.spv" "$D/se2.stock.spv" "$D/orig_se2.spvasm" "$D/se2.new.spv.spvasm"
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
    "description": "Chroma-key passthrough for native apps (vrcompositor only)",
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
Comment=Passthrough keying for apps on the Frame; opens its SteamVR dashboard panel
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
