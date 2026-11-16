#!/bin/sh
# keyer: chroma-key passthrough for native apps on the Steam Frame. Steam Link
# streams are left alone (SteamOS 0.5.9 keys those itself).
#
#   keyer.sh build                   compile everything into build/ (needs ~/sdk, see README)
#   keyer.sh chroma R G B [thr soft spill margin]   key this colour (0..1), e.g. chroma 0 1 0
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
# Remove every keyed copy (and the stock shader each was made from). se2 is the
# stream shader's copy that releases before 0.7 made.
unkey() { for n in alphaed achromatic fsharp se2; do rm -f "$DATA/$n.spv" "$DATA/$n.stock.spv"; done; }
# keyed copy of one compositor shader: key NAME STOCKFILE
# Next to it goes a copy of the stock shader it was made from: the layer only
# hands over the keyed copy while SteamVR's shader is still that one, since a
# copy made for an older SteamVR reads inputs the new compositor doesn't provide.
key() {
	n=$1 f=$2
	rm -f "$DATA/$n.spv" "$DATA/$n.stock.spv"
	spirv-dis "$SHADERS/$f" -o "$DATA/orig_$n.spvasm" || return 1
	python3 gen_key.py "$DATA/orig_$n.spvasm" "$DATA/$n.new.spv" chroma >/dev/null || return 1
	cp "$SHADERS/$f" "$DATA/$n.stock.spv" || return 1
	mv "$DATA/$n.new.spv" "$DATA/$n.spv"
}
# A shader can't be keyed: leave the compositor stock and say so. Keying only
# some of them would show a sharp keyed circle on an unkeyed view, or the
# other way round.
fail() {
	unkey
	rm -f "$DATA/stamp" "$DATA/mode"
	blend remove
	reload
	echo "native app shaders not recognised (SteamVR update?): keying stays off" >&2
	exit 1
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
	# test app (native OpenXR: key colour with a grey square); needs the OpenXR loader in ~/sdk/prefix.
	# Not shipped, so a failed build only warns (a loader built on an older SteamOS can stop linking).
	if [ -d "$SDK/prefix/include/openxr" ]; then
		g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -I"$SDK/Vulkan-Headers/include" \
			-I"$SDK/prefix/include" xrkeytest.cpp -o build/xrkeytest "$SDK/prefix/lib/libopenxr_loader.so" \
			-lvulkan -Wl,-rpath,"$SDK/prefix/lib" || echo "xrkeytest not built (test app only)" >&2
	fi
	echo built ;;
chroma)
	# The keyed shaders read the colour and sliders at draw time from the params
	# file (gen_key.py, key_layer.cpp). Only turning keying on, or a new
	# gen_key.py or SteamVR shader, needs a shader reload.
	shift
	python3 gen_key.py params "$DATA/params" "$@"
	stamp=$(cat gen_key.py "$SHADERS/distort_ps_achromatic_alphaed.spv" "$SHADERS/distort_ps_achromatic.spv" \
		"$SHADERS/fsharp_vr_ps.spv" 2>/dev/null | md5sum)
	if [ -f "$DATA/alphaed.spv" ] && [ "$(cat "$DATA/stamp" 2>/dev/null)" = "$stamp" ] && [ -f "$DATA/mode" ]; then
		echo "chroma $*" > "$DATA/mode"
		echo "keying: chroma $*"
		exit 0
	fi
	rm -f "$DATA/stamp"
	unkey
	key alphaed distort_ps_achromatic_alphaed.spv || fail
	key achromatic distort_ps_achromatic.spv || fail
	key fsharp fsharp_vr_ps.spv || fail
	echo "$stamp" > "$DATA/stamp"
	echo "chroma $*" > "$DATA/mode"
	blend true
	reload
	echo "keying: chroma $*" ;;
off)
	unkey
	rm -f "$DATA/mode" "$DATA/params"
	blend remove
	reload
	echo off ;;
status)
	[ -f "$MANIFEST" ] && echo "layer: installed" || echo "layer: not installed"
	[ -f "$DATA/mode" ] && echo "mode: $(cat "$DATA/mode")" || echo "mode: off"
	[ -f "$DATA/layer.log" ] && tail -3 "$DATA/layer.log" ;;
*)
	sed -n '5,8p' "$0"; exit 2 ;;
esac
