// xralphatest: minimal D3D11 OpenXR app that asks for ALPHA_BLEND (the way a
// mixed-reality app would) and draws a mostly transparent scene:
//   - projection layer: transparent, an opaque blue band along the top of each
//     eye and a 50%-transparent band along the bottom
//   - an opaque orange panel 1 m ahead (quad layer)
//
//   xralphatest [seconds]
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { printf("%s -> %d\n", #x, r_); return 1; } } while (0)

struct Chain {
	XrSwapchain h;
	int32_t w, hgt;
	std::vector<ID3D11RenderTargetView *> rtv;
};

static bool makeChain(XrSession s, ID3D11Device1 *dev, int64_t fmt, int32_t w, int32_t h, Chain &c) {
	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	ci.format = fmt;
	ci.sampleCount = 1;
	ci.width = w;
	ci.height = h;
	ci.faceCount = ci.arraySize = ci.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(s, &ci, &c.h))) return false;
	c.w = w;
	c.hgt = h;
	uint32_t n = 0;
	xrEnumerateSwapchainImages(c.h, 0, &n, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	xrEnumerateSwapchainImages(c.h, n, &n, (XrSwapchainImageBaseHeader *)imgs.data());
	for (auto &i : imgs) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {(DXGI_FORMAT)fmt, D3D11_RTV_DIMENSION_TEXTURE2D};
		ID3D11RenderTargetView *v = nullptr;
		dev->CreateRenderTargetView(i.texture, &rd, &v);
		c.rtv.push_back(v);
	}
	return true;
}

int main(int argc, char **argv) {
	int seconds = argc > 1 ? atoi(argv[1]) : 60;
	const char *ext[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ici.applicationInfo.applicationName, "xralphatest");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = ext;
	XrInstance inst;
	CHECK(xrCreateInstance(&ici, &inst));
	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	CHECK(xrGetSystem(inst, &sgi, &sys));

	const XrViewConfigurationType vct = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	uint32_t nb = 0;
	xrEnumerateEnvironmentBlendModes(inst, sys, vct, 0, &nb, nullptr);
	std::vector<XrEnvironmentBlendMode> modes(nb);
	xrEnumerateEnvironmentBlendModes(inst, sys, vct, nb, &nb, modes.data());
	XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	printf("blend modes:");
	for (auto m : modes) {
		printf(" %d", m);
		if (m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) blend = m;
	}
	printf(" -> using %s\n", blend == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "ALPHA_BLEND" : "OPAQUE");

	PFN_xrGetD3D11GraphicsRequirementsKHR getReq;
	CHECK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
	XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	CHECK(getReq(inst, sys, &req));
	IDXGIFactory1 *fac;
	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
	IDXGIAdapter1 *ad = nullptr;
	for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 d;
		ad->GetDesc1(&d);
		if (!memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof d.AdapterLuid)) break;
		ad->Release();
		ad = nullptr;
	}
	ID3D11Device *dev0;
	ID3D11DeviceContext *ctx0;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	if (FAILED(D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
		    D3D11_SDK_VERSION, &dev0, nullptr, &ctx0))) { printf("D3D11CreateDevice failed\n"); return 1; }
	ID3D11Device1 *dev;
	ID3D11DeviceContext1 *ctx;
	dev0->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev);
	ctx0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx);

	XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb.device = dev;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &gb;
	sci.systemId = sys;
	XrSession s;
	CHECK(xrCreateSession(inst, &sci, &s));
	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rci.poseInReferenceSpace.orientation.w = 1;
	XrSpace space;
	CHECK(xrCreateReferenceSpace(s, &rci, &space));

	uint32_t nv = 0;
	xrEnumerateViewConfigurationViews(inst, sys, vct, 0, &nv, nullptr);
	std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	xrEnumerateViewConfigurationViews(inst, sys, vct, nv, &nv, vcv.data());
	uint32_t nf = 0;
	xrEnumerateSwapchainFormats(s, 0, &nf, nullptr);
	std::vector<int64_t> fmts(nf);
	xrEnumerateSwapchainFormats(s, nf, &nf, fmts.data());
	int64_t fmt = fmts[0];
	for (auto f : fmts)
		if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = f;
	std::vector<Chain> eyes(nv);
	for (uint32_t i = 0; i < nv; i++)
		if (!makeChain(s, dev, fmt, vcv[i].recommendedImageRectWidth, vcv[i].recommendedImageRectHeight, eyes[i])) {
			printf("eye swapchain failed\n");
			return 1;
		}
	Chain panel;
	if (!makeChain(s, dev, fmt, 256, 256, panel)) { printf("panel swapchain failed\n"); return 1; }
	printf("format %lld, eye %dx%d\n", (long long)fmt, eyes[0].w, eyes[0].hgt);

	bool running = false, quit = false;
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!quit) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				auto st = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (st == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = vct;
					xrBeginSession(s, &bi);
					running = true;
				} else if (st == XR_SESSION_STATE_STOPPING) {
					xrEndSession(s);
					running = false;
				} else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING)
					quit = true;
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (std::chrono::steady_clock::now() > end) {
			if (running) xrRequestExitSession(s);
			else quit = true;
		}
		if (!running) { Sleep(10); continue; }

		XrFrameState fs = {XR_TYPE_FRAME_STATE};
		xrWaitFrame(s, nullptr, &fs);
		xrBeginFrame(s, nullptr);
		XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
		vli.viewConfigurationType = vct;
		vli.displayTime = fs.predictedDisplayTime;
		vli.space = space;
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		std::vector<XrView> views(nv, {XR_TYPE_VIEW});
		xrLocateViews(s, &vli, &vs, nv, &nv, views.data());

		std::vector<XrCompositionLayerProjectionView> pv(nv, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
		for (uint32_t i = 0; i < nv; i++) {
			Chain &c = eyes[i];
			uint32_t idx;
			xrAcquireSwapchainImage(c.h, nullptr, &idx);
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(c.h, &wi);
			const float clear[4] = {0, 0, 0, 0}, blue[4] = {0.1f, 0.3f, 0.9f, 1}, half[4] = {0.05f, 0.15f, 0.45f, 0.5f};
			ctx->ClearRenderTargetView(c.rtv[idx], clear);
			D3D11_RECT top = {0, 0, c.w, c.hgt / 5}, bottom = {0, c.hgt * 4 / 5, c.w, c.hgt};
			ctx->ClearView(c.rtv[idx], blue, &top, 1);
			ctx->ClearView(c.rtv[idx], half, &bottom, 1); // premultiplied: 50% of the blue
			xrReleaseSwapchainImage(c.h, nullptr);
			pv[i].pose = views[i].pose;
			pv[i].fov = views[i].fov;
			pv[i].subImage.swapchain = c.h;
			pv[i].subImage.imageRect = {{0, 0}, {c.w, c.hgt}};
		}
		{
			uint32_t idx;
			xrAcquireSwapchainImage(panel.h, nullptr, &idx);
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(panel.h, &wi);
			const float orange[4] = {1, 0.45f, 0.05f, 1};
			ctx->ClearRenderTargetView(panel.rtv[idx], orange);
			xrReleaseSwapchainImage(panel.h, nullptr);
		}
		XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		proj.space = space;
		proj.viewCount = nv;
		proj.views = pv.data();
		XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.space = space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = panel.h;
		quad.subImage.imageRect = {{0, 0}, {panel.w, panel.hgt}};
		quad.pose.orientation.w = 1;
		quad.pose.position = {0, 0, -1};
		quad.size = {0.4f, 0.3f};
		const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&proj,
			(XrCompositionLayerBaseHeader *)&quad};
		XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
		fe.displayTime = fs.predictedDisplayTime;
		fe.environmentBlendMode = blend;
		fe.layerCount = fs.shouldRender ? 2 : 0;
		fe.layers = layers;
		xrEndFrame(s, &fe);
	}
	xrDestroySession(s);
	xrDestroyInstance(inst);
	printf("done\n");
	return 0;
}
