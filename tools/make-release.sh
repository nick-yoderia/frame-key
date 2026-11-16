#!/bin/sh
# Build and zip a release (run on the Windows PC, in Git Bash).
#   sh tools/make-release.sh v0.7.0-alpha
# Everything is built on a Frame over ssh (FRAME_HOST, default "frame"), which
# needs ~/sdk/openvr and ~/sdk/Vulkan-Headers (SteamOS strips its own headers).
# The PC only gets the scripts that install it from there.
set -eu
VER=${1:?version, e.g. v0.7.0-alpha}
H=${FRAME_HOST:-frame}
cd "$(dirname "$0")/.."
OUT=dist/frame-key-$VER
rm -rf "$OUT" && mkdir -p "$OUT/pc" "$OUT/headset/bin"

cp pc/install-headset.ps1 pc/remove-pc-side.ps1 "$OUT/pc/"

R='~/dev/frame-key'
tar -cf - --exclude=__pycache__ --exclude=build headset |
	ssh "$H" "rm -rf $R && mkdir -p $R && tar -xf - -C $R && cd $R/headset && sh keyer.sh build >/dev/null"
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
