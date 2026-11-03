#!/bin/sh
# keyer: chroma-key passthrough on the Steam Frame, for Steam Link streams and
# native apps.
#
#   keyer.sh build                   compile everything into build/ (needs ~/sdk, see README)
#   keyer.sh chroma R G B [thr soft spill margin]   key this colour (0..1), e.g. chroma 1 0 1
#   keyer.sh rect X0 Y0 X1 Y1        passthrough inside a fixed eye rectangle (stream only, test)
#   keyer.sh off                     stock compositor (layer stays loaded, does nothing)
#   keyer.sh status
set -eu
cd "$(dirname "$0")"
SDK=$HOME/sdk
VR=/opt/steamvr/bin/linuxarm64
DATA=$HOME/.local/share/frame-keyer
MANIFEST=$HOME/.local/share/vulkan/implicit_layer.d/frame-keyer.json
SHADERS=/opt/steamvr/resources/shaders/vulkan
# Installed copies keep their programs in bin/, a source checkout builds into build/.
if [ -d bin ]; then BIN=bin; else BIN=build; fi
mkdir -p "$DATA"

reload() { LD_LIBRARY_PATH=$VR timeout 10 $VR/vrcmd --reload-shaders >/dev/null 2>&1 || true; }
blend() { ./$BIN/vrsetting camera forceSceneAlphaBlending "$1"; }
# keyed copy of one compositor shader: key NAME STOCKFILE ARGS...
key() {
	n=$1 f=$2
	shift 2
	spirv-dis "$SHADERS/$f" -o "$DATA/orig_$n.spvasm"
	python3 gen_key.py "$DATA/orig_$n.spvasm" "$DATA/$n.new.spv" "$@" >/dev/null
	mv "$DATA/$n.new.spv" "$DATA/$n.spv"
}

case "${1:-status}" in
build)
	mkdir -p build
	cp "$SDK/openvr/bin/linuxarm64/libopenvr_api.so" build/
	g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-missing-field-initializers \
		-I"$SDK/Vulkan-Headers/include" key_layer.cpp -o build/libVkLayer_frame_keyer.so
	# Programs find libopenvr_api.so next to themselves.
	g++ -std=c++17 -O2 -Wall -I"$SDK/openvr/headers" vrsetting.cpp -o build/vrsetting \
		-Lbuild -lopenvr_api -Wl,-rpath,'$ORIGIN'
	g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -I"$SDK/openvr/headers" -I"$SDK/Vulkan-Headers/include" \
		-I/usr/include/freetype2 panel.cpp vkshare.cpp -o build/frame-keyer-panel \
		-Lbuild -lopenvr_api -lvulkan -lfreetype -lpthread -Wl,-rpath,'$ORIGIN'
	# test app (native OpenXR: key colour with a grey square); needs the OpenXR loader in ~/sdk/prefix
	if [ -d "$SDK/prefix/include/openxr" ]; then
		g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -I"$SDK/Vulkan-Headers/include" \
			-I"$SDK/prefix/include" xrkeytest.cpp -o build/xrkeytest "$SDK/prefix/lib/libopenxr_loader.so" \
			-lvulkan -Wl,-rpath,"$SDK/prefix/lib"
	fi
	echo built ;;
chroma)
	# The keyed shaders read the colour and sliders at draw time from the params
	# file (gen_key.py, key_layer.cpp). Only turning keying on, or a new
	# gen_key.py or SteamVR shader, needs a shader reload.
	shift
	python3 gen_key.py params "$DATA/params" "$@"
	stamp=$(cat gen_key.py "$SHADERS/distort_ps_achromatic_se2.spv" "$SHADERS/distort_ps_achromatic_alphaed.spv" \
		"$SHADERS/distort_ps_achromatic.spv" "$SHADERS/fsharp_vr_ps.spv" 2>/dev/null | md5sum)
	if [ -f "$DATA/se2.spv" ] && [ "$(cat "$DATA/stamp" 2>/dev/null)" = "$stamp" ] &&
		case $(cat "$DATA/mode" 2>/dev/null) in chroma*) true ;; *) false ;; esac; then
		echo "chroma $*" > "$DATA/mode"
		echo "keying: chroma $*"
		exit 0
	fi
	rm -f "$DATA/stamp"
	key se2 distort_ps_achromatic_se2.spv chroma
	# native apps get the same key
	rm -f "$DATA/alphaed.spv" "$DATA/achromatic.spv" "$DATA/fsharp.spv"
	key alphaed distort_ps_achromatic_alphaed.spv chroma || echo "native apps not keyed (shader changed?)" >&2
	key achromatic distort_ps_achromatic.spv chroma || echo "native apps without alpha not keyed (shader changed?)" >&2
	key fsharp fsharp_vr_ps.spv chroma || echo "native apps' sharp circle not keyed (shader changed?)" >&2
	echo "$stamp" > "$DATA/stamp"
	echo "chroma $*" > "$DATA/mode"
	blend true
	reload
	echo "keying: chroma $*" ;;
rect)
	key se2 distort_ps_achromatic_se2.spv "$@"
	# a stream-only test
	rm -f "$DATA/alphaed.spv" "$DATA/achromatic.spv" "$DATA/fsharp.spv"
	echo "$*" > "$DATA/mode"
	blend true
	reload
	echo "keying: $*" ;;
off)
	rm -f "$DATA/se2.spv" "$DATA/alphaed.spv" "$DATA/achromatic.spv" "$DATA/fsharp.spv" "$DATA/mode" "$DATA/params"
	blend remove
	reload
	echo off ;;
status)
	[ -f "$MANIFEST" ] && echo "layer: installed" || echo "layer: not installed"
	[ -f "$DATA/mode" ] && echo "mode: $(cat "$DATA/mode")" || echo "mode: off"
	[ -f "$DATA/layer.log" ] && tail -3 "$DATA/layer.log" ;;
*)
	sed -n '5,9p' "$0"; exit 2 ;;
esac
