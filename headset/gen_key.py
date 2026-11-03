#!/usr/bin/env python3
"""Make a keyed copy of one of vrcompositor's shaders: the Steam Link stream's
(distort_ps_achromatic_se2) or the two that draw native Frame apps
(distort_ps_achromatic_alphaed for the whole view, or distort_ps_achromatic
for apps without alpha; fsharp_vr_ps for the sharp eye-tracked circle). Which one it is gets detected from the shader.

With foveated streaming, each eye of the Steam Link stream is two pictures:
a sharp inset where you look and a low-res picture of the whole view. The
stock shader samples both (inset %1003, periphery %1007), blends them (%687,
inset weight %1008 fading out near the inset's edge), fades the eye's outer
edge to black (%899), tints, and stores alpha 1 (`OpStore %504 %994`).

The keyed copy tests the blended colour %687 against the key, so the inset and
the periphery are keyed alike, and stores premultiplied (rgb * a, a); the
compositor's passthrough pass fills in where a < 1. Pixels near the key also
lose the key's tint (despill), so the half-key fringe that video compression
smears around a keyed shape turns neutral instead of pink.

alphaed samples the app's picture (%187, linear light since app swapchains are
sRGB), tints it (%196) and stores it with the app's alpha (`OpStore %135 %256`).
The keyed copy compares the colour in sRGB, like the stream and the panel do,
and multiplies the app's alpha by the key's: the compositor draws native apps
in more than one piece (eye-tracked foveation) and blends them by that alpha.
distort_ps_achromatic is the same with alpha 1 (%255), other ids.
fsharp_vr_ps sharpens the app's picture into %935, tints it (%938) and stores
it with the circle's edge fade %873 as alpha (`OpStore %880 %2762`); keyed the
same way, the circle turns transparent over the key and the keyed view below
shows.

  gen_key.py ORIG.spvasm OUT.spv chroma               keyed copy, settings read at draw time
  gen_key.py params OUT R G B [threshold softness spill spill_margin]   write those settings
  gen_key.py params OUT hold                           settings that key nothing (alpha 1, no despill)
  gen_key.py ORIG.spvasm OUT.spv rect X0 Y0 X1 Y1     (se2 only: passthrough inside, 0..1 of each eye)

The chroma copy doesn't contain the key colour or the sliders: it reads them
from push constants, which the layer (key_layer.cpp) fills from the `params`
file every time the compositor binds the shader. Changing a setting then only
rewrites that file; a shader reload makes the compositor rebuild every
pipeline, and many of those in a row while a heavy app ran once stalled it
until the system killed it.

Alpha ramps linearly from 0 at `threshold` to 1 at `threshold + softness`
(BT.709 CbCr distance from the key; pure magenta to grey is about 0.59). The
PC layer (pc/alphakey) writes partial alpha onto that same ramp, so keep its
`ramp` setting equal to these two numbers. Despill reaches `spill_margin`
past the ramp's end, so only colours bordering the key lose its tint (a fixed
reach turned orange yellow under a red key).

Native apps (video) are keyed on CbCr divided by brightness (max channel +
NORM_EPS) instead, so every shade of the key's colour matches it: filmed green
screens are darker and duller than pure green, and with plain CbCr the Green
preset missed them while a dull eyedropper pick sat close to every grey, white
and black. Near-black pixels (max channel below DARK) stay solid, since their
colour is mostly compression noise. The stream keeps plain CbCr because the PC
layer's partial alpha is written onto that ramp.
"""
import math
import os
import struct
import subprocess
import sys

# Ids in the SteamVR 2.18.x shaders (SteamOS 0.5.6). Checked before patching.
FUNC = "%4 = OpFunction %void None %3"
COMMON = [FUNC, '%1 = OpExtInstImport "GLSL.std.450"']
SE2 = {
	"store": "OpStore %504 %994",
	"anchors": [
		"%687 = OpFAdd %v3float %682 %686",  # inset/periphery blend, before edge fade and tint
		"%899 = OpFAdd %float",              # eye edge fade
		"%907 = OpVectorShuffle %v3float",   # tint
		"%883 = OpFDiv %v2float",            # position in the scene layer, 0..1
		"%911 = OpFMul %v3float",            # final tinted rgb
	],
	"out": "%504",
}
ALPHAED = {
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
ACHROMATIC = {
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
FSHARP = {
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
NATIVE = (ALPHAED, ACHROMATIC, FSHARP)


def variant(asm):
	for v in (SE2,) + NATIVE:
		if all(asm.count(m) == 1 for m in COMMON + [v["store"]] + v["anchors"]):
			return v
	sys.exit("unexpected shader layout (SteamVR update?): not a shader this keyer knows")
CB, CR = 1.8556, 1.5748  # BT.709: Cb = (B - Y) / CB, Cr = (R - Y) / CR
LUMA = (0.2126, 0.7152, 0.0722)
NORM_EPS = 0.1     # native: CbCr / (max channel + NORM_EPS)
DARK = (0.09, 0.15)  # native: solid below the first max-channel value, fading to keyable at the second


# Push constants, one float each, in this order. The params file holds two such
# blocks (stream, native apps) after a 16-byte header; the layer pushes PC_BYTES.
PC = ["kkb", "kkr", "kdb", "kdr", "kt0", "kisoft", "kte", "kts", "ksp"]
PC_BYTES = 64
MAGIC = b"FKP1"


def cbcr(r, g, b):
	y = LUMA[0] * r + LUMA[1] * g + LUMA[2] * b
	return (b - y) / CB, (r - y) / CR


def f(x):
	return repr(float(x))


def rect(a):
	x0, y0, x1, y1 = a[:4]
	consts = ["%kx0 = OpConstant %float " + f(x0), "%ky0 = OpConstant %float " + f(y0),
		"%kx1 = OpConstant %float " + f(x1), "%ky1 = OpConstant %float " + f(y1)]
	code = [
		"%kux = OpCompositeExtract %float %883 0",
		"%kuy = OpCompositeExtract %float %883 1",
		"%kc0 = OpFOrdGreaterThanEqual %bool %kux %kx0",
		"%kc1 = OpFOrdLessThan %bool %kux %kx1",
		"%kc2 = OpFOrdGreaterThanEqual %bool %kuy %ky0",
		"%kc3 = OpFOrdLessThan %bool %kuy %ky1",
		"%kcx = OpLogicalAnd %bool %kc0 %kc1",
		"%kcy = OpLogicalAnd %bool %kc2 %kc3",
		"%kin = OpLogicalAnd %bool %kcx %kcy",
		"%ka = OpSelect %float %kin %kf0 %kf1",
	]
	return [], consts, code, "%911"


def settings(a, native):
	"""The push constant values (PC order) for chroma R G B [thr soft spill margin]."""
	r, g, b = a[:3]
	thr, soft, spill, margin = (a[3:] + [0.12, 0.08, 1.0, 0.05][len(a) - 3:])[:4]
	soft = max(soft, 1e-4)
	kb0, kr0 = cbcr(r, g, b)
	n = math.hypot(kb0, kr0) or 1.0
	kv = max(r, g, b) + NORM_EPS if native else 1.0  # native: the key's brightness-normalised CbCr
	return [kb0 / kv, kr0 / kv, kb0 / n, kr0 / n, thr, 1 / soft, thr + soft, thr + soft + max(margin, 1e-4), spill]


# Keys nothing: alpha = clamp((d + 1000) * 1) = 1 and despill weight 0.
HOLD = [0.0, 0.0, 1.0, 0.0, -1000.0, 1.0, -1001.0, -1000.0, 0.0]


def write_params(out, a):
	blocks = [HOLD, HOLD] if a == ["hold"] else [settings([float(x) for x in a], native) for native in (False, True)]
	data = MAGIC + bytes(12) + b"".join(struct.pack("<%df" % len(b), *b).ljust(PC_BYTES, bytes(1)) for b in blocks)
	open(out + ".tmp", "wb").write(data)
	os.replace(out + ".tmp", out)


def chroma(v):
	native = v is not SE2
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
	]
	code = []
	for i, name in enumerate(PC):
		code += ["%%%sp = OpAccessChain %%kPCf %%kpc %%kpi%d" % (name, i), "%%%s = OpLoad %%float %%%sp" % (name, name)]
	code += ["%kkey = OpCompositeConstruct %v2float %kkb %kkr", "%kdir = OpCompositeConstruct %v2float %kdb %kdr"]
	if not native:
		src = "%687"  # the blended stream colour
	else:
		# the app's colour, linear -> sRGB (gamma 2.2 is close enough for a key)
		consts += ["%kg = OpConstant %float 0.454545", "%kgi = OpConstant %float 2.2",
			"%kgv = OpConstantComposite %v3float %kg %kg %kg", "%kgiv = OpConstantComposite %v3float %kgi %kgi %kgi",
			"%kz3 = OpConstantComposite %v3float %kf0 %kf0 %kf0", "%kev = OpConstant %float " + f(NORM_EPS),
			"%kdk0 = OpConstant %float " + f(DARK[0]), "%kdk1 = OpConstant %float " + f(DARK[1])]
		src = "%ksrgb"
		code += ["%klin = OpExtInst %v3float %1 FMax " + v["rgb"] + " %kz3",
			"%ksrgb = OpExtInst %v3float %1 Pow %klin %kgv"]
	code += [
		# key test
		"%ky = OpDot %float " + src + " %kluma",
		"%kr = OpCompositeExtract %float " + src + " 0",
		"%kb = OpCompositeExtract %float " + src + " 2",
		"%kbmy = OpFSub %float %kb %ky",
		"%kcb = OpFMul %float %kbmy %kicb",
		"%krmy = OpFSub %float %kr %ky",
		"%kcr = OpFMul %float %krmy %kicr",
		"%kcbcr = OpCompositeConstruct %v2float %kcb %kcr",
	]
	if native:
		code += [  # CbCr / (max channel + NORM_EPS)
			"%kgc = OpCompositeExtract %float " + src + " 1",
			"%kmx0 = OpExtInst %float %1 FMax %kr %kgc",
			"%kmx = OpExtInst %float %1 FMax %kmx0 %kb",
			"%kvv = OpFAdd %float %kmx %kev",
			"%kivv = OpFDiv %float %kf1 %kvv",
			"%kcbcrn = OpVectorTimesScalar %v2float %kcbcr %kivv",
			"%kd = OpExtInst %float %1 Distance %kcbcrn %kkey",
		]
	else:
		code += ["%kd = OpExtInst %float %1 Distance %kcbcr %kkey"]
	code += [
		# linear ramp, so partial alpha sent by the PC layer comes back as sent
		"%kdt = OpFSub %float %kd %kt0",
		"%kdq = OpFMul %float %kdt %kisoft",
		"%ka" + ("0" if native else "") + " = OpExtInst %float %1 FClamp %kdq %kf0 %kf1",
	]
	if native:
		code += [  # near-black stays solid
			"%kdg = OpExtInst %float %1 SmoothStep %kdk0 %kdk1 %kmx",
			"%kdgi = OpFSub %float %kf1 %kdg",
			"%ka = OpExtInst %float %1 FMax %ka0 %kdgi",
		]
	code += [
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
	]
	if v is SE2:
		# the stock edge fade and tint, applied to the despilled colour
		code += ["%kfade = OpVectorTimesScalar %v3float %krgb %899", "%kfin = OpFMul %v3float %kfade %907"]
	else:
		# back to linear, then the stock tint
		code += ["%krgb0 = OpExtInst %v3float %1 FMax %krgb %kz3", "%klin2 = OpExtInst %v3float %1 Pow %krgb0 %kgiv",
			"%kfin = OpFMul %v3float %klin2 " + v["tint"]]
	return decor, consts, code, "%kfin"


def patch(asm, mode, a):
	v = variant(asm)
	if mode not in ("rect", "chroma") or (mode == "rect" and v is not SE2):
		sys.exit("mode must be chroma, or rect for the stream shader")
	decor, consts, code, rgb = rect(a) if mode == "rect" else chroma(v)
	consts = ["%kf0 = OpConstant %float 0", "%kf1 = OpConstant %float 1"] + consts
	code += [
		"%kprgb = OpVectorTimesScalar %v3float " + rgb + " %ka",
		"%ko0 = OpCompositeExtract %float %kprgb 0",
		"%ko1 = OpCompositeExtract %float %kprgb 1",
		"%ko2 = OpCompositeExtract %float %kprgb 2",
	]
	alpha = "%ka"
	if v in NATIVE:
		code += v["alpha"] + ["%kab = OpFMul %float %ka %kaa"]
		alpha = "%kab"
	code += [
		"%kout = OpCompositeConstruct %v4float %ko0 %ko1 %ko2 " + alpha,
		"OpStore " + v["out"] + " %kout",
	]
	asm = asm.replace(FUNC, "\n".join(consts) + "\n" + FUNC)
	if decor:  # with the other decorations, before any type
		i = asm.rindex("\n", 0, asm.index("OpDecorate")) + 1
		asm = asm[:i] + "\n".join(decor) + "\n" + asm[i:]
	return asm.replace(v["store"], "\n".join(code))


def main():
	if len(sys.argv) >= 3 and sys.argv[1] == "params":
		return write_params(sys.argv[2], sys.argv[3:])
	if len(sys.argv) < 4:
		sys.exit(__doc__)
	src, out, mode = sys.argv[1:4]
	asm = patch(open(src).read(), mode, [float(x) for x in sys.argv[4:]])
	tmp = out + ".spvasm"
	open(tmp, "w").write(asm)
	subprocess.run(["spirv-as", "--target-env", "vulkan1.1", tmp, "-o", out], check=True)
	subprocess.run(["spirv-val", "--target-env", "vulkan1.1", out], check=True)
	print("wrote", out)


if __name__ == "__main__":
	main()
