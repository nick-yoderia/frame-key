#!/usr/bin/env python3
"""Make a keyed copy of one of the vrcompositor shaders that draw native Frame
apps: distort_ps_achromatic_alphaed for the whole view (distort_ps_achromatic
for apps without alpha) and fsharp_vr_ps for the sharp eye-tracked circle.
Which one it is gets detected from the shader. The Steam Link stream's shader
isn't touched: SteamOS 0.5.9 keys PC streams itself.

alphaed samples the app's picture (linear light since app swapchains are
sRGB), tints it and stores it with the app's alpha. The keyed copy compares the
colour in sRGB, like the panel does, multiplies the app's alpha by the key's
and stores premultiplied (rgb * a, a); the compositor's passthrough pass fills
in where a < 1. The compositor draws native apps in more than one piece
(eye-tracked foveation) and blends them by that alpha. distort_ps_achromatic is
the same with alpha 1. fsharp_vr_ps sharpens the app's picture, tints it and
stores it with the circle's edge fade as alpha; keyed the same way, the circle
turns transparent over the key and the keyed view below shows. Pixels near the
key also lose the key's tint (despill), so the half-key fringe that video
compression smears around a keyed shape turns neutral.

Which SSA ids hold those values differs per SteamOS release: see the tables
below. A shader matching none of them is refused (exit 1).

  gen_key.py ORIG.spvasm OUT.spv chroma               keyed copy, settings read at draw time
  gen_key.py params OUT R G B [threshold softness spill spill_margin]   write those settings
  gen_key.py params OUT hold                           settings that key nothing (alpha 1, no despill)

The keyed copy doesn't contain the key colour or the sliders: it reads them
from push constants, which the layer (key_layer.cpp) fills from the `params`
file every time the compositor binds the shader. Changing a setting then only
rewrites that file; a shader reload makes the compositor rebuild every
pipeline, and many of those in a row while a heavy app ran once stalled it
until the system killed it.

Alpha ramps linearly from 0 at `threshold` to 1 at `threshold + softness`.
The distance is BT.709 CbCr divided by brightness (max channel + NORM_EPS), so
every shade of the key's colour matches it: filmed green screens are darker
and duller than pure green, and with plain CbCr the Green preset missed them
while a dull eyedropper pick sat close to every grey, white and black.
Near-black pixels (max channel below DARK) stay solid, since their colour is
mostly compression noise. Despill reaches `spill_margin` past the ramp's end,
so only colours bordering the key lose its tint (a fixed reach turned orange
yellow under a red key).
"""
import math
import os
import struct
import subprocess
import sys

# Ids in the stock shaders, one table per SteamOS release (each update renumbers
# them). Every anchor must occur exactly once, or the shader isn't patched: a
# keyed copy built for other ids would read inputs that aren't there.
FUNC = "%4 = OpFunction %void None %3"
COMMON = [FUNC, '%1 = OpExtInstImport "GLSL.std.450"']

# --- SteamOS 0.5.7 to 0.5.9 ---
ALPHAED = {
	"store": "OpStore %136 %257",
	"anchors": [
		"%188 = OpImageSampleImplicitLod %v4float",       # the app's pixel
		"%197 = OpVectorShuffle %v3float %196 %196 0 1 2",  # tint
		"%200 = OpVectorShuffle %v3float %188 %188 0 1 2",  # its rgb
		"%136 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%136",
	"rgb": "%200", "tint": "%197",
	"alpha": ["%kaa = OpCompositeExtract %float %188 3"],  # the app's alpha
}
ACHROMATIC = {
	"store": "OpStore %138 %262",
	"anchors": [
		"%256 = OpCompositeInsert %v4float %float_1 %190 3",  # the app's pixel, alpha 1
		"%200 = OpVectorShuffle %v3float %199 %199 0 1 2",    # tint
		"%203 = OpVectorShuffle %v3float %256 %256 0 1 2",    # its rgb
		"%138 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%138",
	"rgb": "%203", "tint": "%200",
	"alpha": ["%kaa = OpCopyObject %float %kf1"],
}
FSHARP = {
	"store": "OpStore %881 %2763",
	"anchors": [
		"%936 = OpLoad %v4float %925",                      # sharpened app pixel
		"%939 = OpVectorShuffle %v3float %938 %938 0 1 2",  # tint
		"%941 = OpVectorShuffle %v3float %936 %936 0 1 2",  # its rgb
		"%874 = OpLoad %float %873",                        # edge fade of the circle
		"%881 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%881",
	"rgb": "%941", "tint": "%939",
	"alpha": ["%kaa = OpCopyObject %float %874"],
}

# --- SteamOS 0.5.6 (SteamVR 2.18.x) ---
ALPHAED_056 = {
	"store": "OpStore %135 %256",
	"anchors": [
		"%187 = OpImageSampleImplicitLod %v4float",       # the app's pixel
		"%196 = OpVectorShuffle %v3float %195 %195 0 1 2",  # tint
		"%199 = OpVectorShuffle %v3float %187 %187 0 1 2",  # its rgb
		"%135 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%135",
	"rgb": "%199", "tint": "%196",
	"alpha": ["%kaa = OpCompositeExtract %float %187 3"],  # the app's alpha
}
ACHROMATIC_056 = {
	"store": "OpStore %137 %261",
	"anchors": [
		"%255 = OpCompositeInsert %v4float %float_1 %189 3",  # the app's pixel, alpha 1
		"%199 = OpVectorShuffle %v3float %198 %198 0 1 2",    # tint
		"%202 = OpVectorShuffle %v3float %255 %255 0 1 2",    # its rgb
		"%137 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%137",
	"rgb": "%202", "tint": "%199",
	"alpha": ["%kaa = OpCopyObject %float %kf1"],
}
FSHARP_056 = {
	"store": "OpStore %880 %2762",
	"anchors": [
		"%935 = OpLoad %v4float %924",                      # sharpened app pixel
		"%938 = OpVectorShuffle %v3float %937 %937 0 1 2",  # tint
		"%940 = OpVectorShuffle %v3float %935 %935 0 1 2",  # its rgb
		"%873 = OpLoad %float %872",                        # edge fade of the circle
		"%880 = OpVariable %_ptr_Output_v4float Output",
	],
	"out": "%880",
	"rgb": "%940", "tint": "%938",
	"alpha": ["%kaa = OpCopyObject %float %873"],
}
VARIANTS = (ALPHAED, ACHROMATIC, FSHARP, ALPHAED_056, ACHROMATIC_056, FSHARP_056)


def variant(asm):
	for v in VARIANTS:
		if all(asm.count(m) == 1 for m in COMMON + [v["store"]] + v["anchors"]):
			return v
	sys.exit("unexpected shader layout (SteamVR update?): not a shader this keyer knows")


CB, CR = 1.8556, 1.5748  # BT.709: Cb = (B - Y) / CB, Cr = (R - Y) / CR
LUMA = (0.2126, 0.7152, 0.0722)
NORM_EPS = 0.1     # CbCr / (max channel + NORM_EPS)
DARK = (0.09, 0.15)  # solid below the first max-channel value, fading to keyable at the second


# Push constants, one float each, in this order. The params file holds one such
# block after a 16-byte header; the layer pushes PC_BYTES.
PC = ["kkb", "kkr", "kdb", "kdr", "kt0", "kisoft", "kte", "kts", "ksp"]
PC_BYTES = 64
MAGIC = b"FKP2"


def cbcr(r, g, b):
	y = LUMA[0] * r + LUMA[1] * g + LUMA[2] * b
	return (b - y) / CB, (r - y) / CR


def f(x):
	return repr(float(x))


def settings(a):
	"""The push constant values (PC order) for chroma R G B [thr soft spill margin]."""
	r, g, b = a[:3]
	thr, soft, spill, margin = (a[3:] + [0.12, 0.08, 1.0, 0.05][len(a) - 3:])[:4]
	soft = max(soft, 1e-4)
	kb0, kr0 = cbcr(r, g, b)
	n = math.hypot(kb0, kr0) or 1.0
	kv = max(r, g, b) + NORM_EPS  # the key's brightness-normalised CbCr
	return [kb0 / kv, kr0 / kv, kb0 / n, kr0 / n, thr, 1 / soft, thr + soft, thr + soft + max(margin, 1e-4), spill]


# Keys nothing: alpha = clamp((d + 1000) * 1) = 1 and despill weight 0.
HOLD = [0.0, 0.0, 1.0, 0.0, -1000.0, 1.0, -1001.0, -1000.0, 0.0]


def write_params(out, a):
	block = HOLD if a == ["hold"] else settings([float(x) for x in a])
	data = MAGIC + bytes(12) + struct.pack("<%df" % len(block), *block).ljust(PC_BYTES, bytes(1))
	open(out + ".tmp", "wb").write(data)
	os.replace(out + ".tmp", out)


def chroma(v):
	# the settings, loaded from the push constant block (see PC)
	decor = ["OpDecorate %kPC Block"] + ["OpMemberDecorate %%kPC %d Offset %d" % (i, 4 * i) for i in range(len(PC))]
	consts = [
		"%kPC = OpTypeStruct " + " ".join(["%float"] * len(PC)),
		"%kPCp = OpTypePointer PushConstant %kPC",
		"%kpc = OpVariable %kPCp PushConstant",
		"%kPCf = OpTypePointer PushConstant %float",
	] + ["%%kpi%d = OpConstant %%uint %d" % (i, i) for i in range(len(PC))] + [
		"%klr = OpConstant %float " + f(LUMA[0]), "%klg = OpConstant %float " + f(LUMA[1]),
		"%klb = OpConstant %float " + f(LUMA[2]),
		"%kluma = OpConstantComposite %v3float %klr %klg %klb",
		"%kicb = OpConstant %float " + f(1 / CB), "%kicr = OpConstant %float " + f(1 / CR),
		"%kmcb = OpConstant %float " + f(CB), "%kmcr = OpConstant %float " + f(CR),
		"%kilg = OpConstant %float " + f(1 / LUMA[1]),
		"%kg = OpConstant %float 0.454545", "%kgi = OpConstant %float 2.2",
		"%kgv = OpConstantComposite %v3float %kg %kg %kg", "%kgiv = OpConstantComposite %v3float %kgi %kgi %kgi",
		"%kz3 = OpConstantComposite %v3float %kf0 %kf0 %kf0", "%kev = OpConstant %float " + f(NORM_EPS),
		"%kdk0 = OpConstant %float " + f(DARK[0]), "%kdk1 = OpConstant %float " + f(DARK[1]),
	]
	code = []
	for i, name in enumerate(PC):
		code += ["%%%sp = OpAccessChain %%kPCf %%kpc %%kpi%d" % (name, i), "%%%s = OpLoad %%float %%%sp" % (name, name)]
	code += [
		"%kkey = OpCompositeConstruct %v2float %kkb %kkr", "%kdir = OpCompositeConstruct %v2float %kdb %kdr",
		# the app's colour, linear -> sRGB (gamma 2.2 is close enough for a key)
		"%klin = OpExtInst %v3float %1 FMax " + v["rgb"] + " %kz3",
		"%ksrgb = OpExtInst %v3float %1 Pow %klin %kgv",
		# key test
		"%ky = OpDot %float %ksrgb %kluma",
		"%kr = OpCompositeExtract %float %ksrgb 0",
		"%kb = OpCompositeExtract %float %ksrgb 2",
		"%kbmy = OpFSub %float %kb %ky",
		"%kcb = OpFMul %float %kbmy %kicb",
		"%krmy = OpFSub %float %kr %ky",
		"%kcr = OpFMul %float %krmy %kicr",
		"%kcbcr = OpCompositeConstruct %v2float %kcb %kcr",
		# CbCr / (max channel + NORM_EPS)
		"%kgc = OpCompositeExtract %float %ksrgb 1",
		"%kmx0 = OpExtInst %float %1 FMax %kr %kgc",
		"%kmx = OpExtInst %float %1 FMax %kmx0 %kb",
		"%kvv = OpFAdd %float %kmx %kev",
		"%kivv = OpFDiv %float %kf1 %kvv",
		"%kcbcrn = OpVectorTimesScalar %v2float %kcbcr %kivv",
		"%kd = OpExtInst %float %1 Distance %kcbcrn %kkey",
		# linear ramp
		"%kdt = OpFSub %float %kd %kt0",
		"%kdq = OpFMul %float %kdt %kisoft",
		"%ka0 = OpExtInst %float %1 FClamp %kdq %kf0 %kf1",
		# near-black stays solid
		"%kdg = OpExtInst %float %1 SmoothStep %kdk0 %kdk1 %kmx",
		"%kdgi = OpFSub %float %kf1 %kdg",
		"%ka = OpExtInst %float %1 FMax %ka0 %kdgi",
		# despill: up to the key's edge, then fading out over spill_margin past it,
		# remove the colour's component along the key's CbCr direction (only the
		# part pointing toward the key)
		"%kws0 = OpExtInst %float %1 SmoothStep %kte %kts %kd",
		"%kws1 = OpFSub %float %kf1 %kws0",
		"%kws = OpFMul %float %kws1 %ksp",
		"%kpj0 = OpDot %float %kcbcr %kdir",
		"%kpj = OpExtInst %float %1 FMax %kpj0 %kf0",
		"%kpw = OpFMul %float %kpj %kws",
		"%kpv = OpVectorTimesScalar %v2float %kdir %kpw",
		"%kc2 = OpFSub %v2float %kcbcr %kpv",
		# back to rgb with the same luma
		"%kcb2 = OpCompositeExtract %float %kc2 0",
		"%kcr2 = OpCompositeExtract %float %kc2 1",
		"%kr2a = OpFMul %float %kcr2 %kmcr",
		"%kr2 = OpFAdd %float %ky %kr2a",
		"%kb2a = OpFMul %float %kcb2 %kmcb",
		"%kb2 = OpFAdd %float %ky %kb2a",
		"%kg2r = OpFMul %float %kr2 %klr",
		"%kg2b = OpFMul %float %kb2 %klb",
		"%kg2s = OpFSub %float %ky %kg2r",
		"%kg2t = OpFSub %float %kg2s %kg2b",
		"%kg2 = OpFMul %float %kg2t %kilg",
		"%krgb = OpCompositeConstruct %v3float %kr2 %kg2 %kb2",
		# back to linear, then the stock tint
		"%krgb0 = OpExtInst %v3float %1 FMax %krgb %kz3",
		"%klin2 = OpExtInst %v3float %1 Pow %krgb0 %kgiv",
		"%kfin = OpFMul %v3float %klin2 " + v["tint"],
		# premultiplied, with the app's alpha times the key's
		"%kprgb = OpVectorTimesScalar %v3float %kfin %ka",
		"%ko0 = OpCompositeExtract %float %kprgb 0",
		"%ko1 = OpCompositeExtract %float %kprgb 1",
		"%ko2 = OpCompositeExtract %float %kprgb 2",
	] + v["alpha"] + [
		"%kab = OpFMul %float %ka %kaa",
		"%kout = OpCompositeConstruct %v4float %ko0 %ko1 %ko2 %kab",
		"OpStore " + v["out"] + " %kout",
	]
	return decor, consts, code


def patch(asm):
	v = variant(asm)
	decor, consts, code = chroma(v)
	consts = ["%kf0 = OpConstant %float 0", "%kf1 = OpConstant %float 1"] + consts
	asm = asm.replace(FUNC, "\n".join(consts) + "\n" + FUNC)
	# with the other decorations, before any type
	i = asm.rindex("\n", 0, asm.index("OpDecorate")) + 1
	asm = asm[:i] + "\n".join(decor) + "\n" + asm[i:]
	return asm.replace(v["store"], "\n".join(code))


def main():
	if len(sys.argv) >= 3 and sys.argv[1] == "params":
		return write_params(sys.argv[2], sys.argv[3:])
	if len(sys.argv) != 4 or sys.argv[3] != "chroma":
		sys.exit(__doc__)
	src, out = sys.argv[1:3]
	asm = patch(open(src).read())
	tmp = out + ".spvasm"
	open(tmp, "w").write(asm)
	subprocess.run(["spirv-as", "--target-env", "vulkan1.1", tmp, "-o", out], check=True)
	subprocess.run(["spirv-val", "--target-env", "vulkan1.1", out], check=True)
	print("wrote", out)


if __name__ == "__main__":
	main()
