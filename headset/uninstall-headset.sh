#!/bin/sh
# Remove the headset side: keying off, panel and layer removed, SteamVR restarted.
#   sh uninstall-headset.sh     (NO_RESTART=1 to skip the SteamVR restart)
set -u
D=$HOME/.local/share/frame-keyer
[ -x "$D/keyer.sh" ] && pgrep -x vrserver >/dev/null && "$D/keyer.sh" off >/dev/null 2>&1
systemctl --user disable --now frame-keyer-panel.service 2>/dev/null
rm -f "$HOME/.config/systemd/user/frame-keyer-panel.service"
systemctl --user daemon-reload
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/frame-keyer.json" "$HOME/.config/frame-keyer/enable" "$HOME/.config/frame-keyer/trace"
rm -f "$HOME/.local/share/applications/frame-keyer.desktop" "$HOME/.local/share/icons/hicolor/128x128/apps/frame-keyer.png"
rm -rf "$D"
echo "Removed. Your panel settings stay in ~/.config/frame-keyer (delete it to forget them)."
if [ -z "${NO_RESTART:-}" ]; then
	echo "Restarting SteamVR..."
	systemctl --user restart steamvr
fi
