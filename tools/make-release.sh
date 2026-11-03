#!/bin/sh
# Build both sides and zip a release (run on the Windows PC, in Git Bash).
#   OPENVR_SDK=... OPENXR_SDK=... sh tools/make-release.sh v0.1.0-alpha
# OPENVR_SDK: ValveSoftware/openvr (headers/, lib/win64/, bin/win64/)
# OPENXR_SDK: the OpenXR.Loader nupkg, unzipped (include/, native/x64/release/)
# The headset side is built on a Frame over ssh (FRAME_HOST, default "frame"),
# which needs ~/sdk/openvr and ~/sdk/Vulkan-Headers (SteamOS strips its own headers).
set -eu
VER=${1:?version, e.g. v0.1.0-alpha}
H=${FRAME_HOST:-frame}
cd "$(dirname "$0")/.."
OUT=dist/frame-key-$VER
rm -rf "$OUT" && mkdir -p "$OUT/pc" "$OUT/headset/bin"

PCFILES="FrameKey.exe frame_alphakey.dll keybox.exe xralphatest.exe openvr_api.dll openxr_loader.dll"
# Remove the last build's outputs first, so a failed step can't ship a stale file.
for f in $PCFILES; do rm -f "pc/build/$f"; done
LOG=$(mktemp)
if ! powershell -NoProfile -ExecutionPolicy Bypass -File pc/build.ps1 -OpenVR "$(cygpath -w "$OPENVR_SDK")" \
	-OpenXR "$(cygpath -w "$OPENXR_SDK")" >"$LOG" 2>&1; then
	cat "$LOG"
	rm -f "$LOG"
	echo "PC build failed" >&2
	exit 1
fi
grep -vE '^\s*\S+\.cpp\s*$|Creating library' "$LOG" || true # cl's per-file chatter
rm -f "$LOG"
for f in $PCFILES; do cp "pc/build/$f" "$OUT/pc/"; done
cp pc/alphakey/alphakey.json pc/alphakey/alphakey.ini pc/install-pc.ps1 pc/install-headset.ps1 "$OUT/pc/"

R='~/dev/frame-key'
tar -cf - headset | ssh "$H" "rm -rf $R && mkdir -p $R && tar -xf - -C $R && cd $R/headset && sh keyer.sh build >/dev/null"
for f in frame-keyer-panel vrsetting libopenvr_api.so; do scp -q "$H:$R/headset/build/$f" "$OUT/headset/bin/"; done
scp -q "$H:$R/headset/build/libVkLayer_frame_keyer.so" "$OUT/headset/"
cp headset/install-headset.sh headset/uninstall-headset.sh headset/keyer.sh headset/gen_key.py \
	headset/frame-keyer-panel.service headset/frame-keyer.png "$OUT/headset/"

cp tools/release/*.cmd tools/release/README.txt LICENSE "$OUT/"
python -I - "$OUT" <<'EOF'
import os, sys, zipfile
out = sys.argv[1]
base = os.path.dirname(out)
with zipfile.ZipFile(out + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
	for root, _, files in os.walk(out):
		for f in files:
			p = os.path.join(root, f)
			info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, base))
			mode = 0o755 if f.endswith((".sh", ".so")) or "bin" in root.split(os.sep) else 0o644
			info.external_attr = (0o100000 | mode) << 16
			info.compress_type = zipfile.ZIP_DEFLATED
			z.writestr(info, open(p, "rb").read())
print("wrote", out + ".zip")
EOF
