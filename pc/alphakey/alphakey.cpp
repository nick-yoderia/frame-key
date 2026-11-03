// XR_APILAYER_frame_alphakey: OpenXR API layer for PC SteamVR + Steam Frame.
//
// SteamVR on the PC has no passthrough, so it offers only OPAQUE. This layer
// also offers XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND. When an app picks it, the
// layer fills the app's transparent pixels with the key colour just before
// each projection image goes back to the runtime, and submits the frame as
// OPAQUE. The Frame's keyer (keyer/) then shows passthrough where the key is.
//
// Content that is already close to the key colour is pushed away from it
// first, so an app can't key by accident, and partly transparent pixels are
// written onto the keyer's alpha ramp. D3D11 apps only for now.
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LAYER_NAME "XR_APILAYER_frame_alphakey"

// ---------------------------------------------------------------- config, log
// The Frame's keyer turns CbCr distance d from the key into alpha:
// 0 below ramp[0], 1 above ramp[1], linear in between. Opaque content is kept
// at least `guard` from the key; partial alpha is written onto the ramp.
static float g_key[3] = {1, 0, 1};
static float g_ramp[2] = {0.12f, 0.20f};
static float g_guard = 0.23f;
static bool g_enabled = true; // enabled = 0 in alphakey.ini: pass everything through untouched
static bool g_guardFromIni = false;
static std::string g_pin = "auto"; // headset = <id> in alphakey.ini, set by the Frame Key app on the PC
static FILE *g_log;
static std::string g_dir; // folder of this DLL

static void logf(const char *fmt, ...) {
	if (!g_log) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
	va_list a;
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

// alphakey.ini next to the DLL: key = RRGGBB, ramp = START END, guard = D
static void loadConfig() {
	HMODULE m;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&loadConfig, &m);
	char p[MAX_PATH];
	GetModuleFileNameA(m, p, MAX_PATH);
	g_dir = p;
	g_dir.resize(g_dir.find_last_of("\\/") + 1);
	if (!g_log) g_log = fopen((g_dir + "alphakey.log").c_str(), "w");
	bool guardSet = false;
	g_pin = "auto";
	if (FILE *f = fopen((g_dir + "alphakey.ini").c_str(), "r")) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			unsigned rgb;
			float v, w;
			if (sscanf(line, " key = %x", &rgb) == 1)
				for (int i = 0; i < 3; i++) g_key[i] = ((rgb >> (16 - 8 * i)) & 255) / 255.0f;
			else if (sscanf(line, " ramp = %f %f", &v, &w) == 2)
				g_ramp[0] = v, g_ramp[1] = w;
			else if (sscanf(line, " guard = %f", &v) == 1)
				g_guard = v, guardSet = true;
			else if (sscanf(line, " enabled = %f", &v) == 1)
				g_enabled = v != 0;
			else {
				char id[64];
				if (sscanf(line, " headset = %63s", id) == 1) g_pin = id;
			}
		}
		fclose(f);
	}
	g_guardFromIni = guardSet;
	if (!guardSet) g_guard = g_ramp[1] + 0.03f;
	logf("%s; key %.3f %.3f %.3f, ramp %.3f..%.3f, guard %.3f", g_enabled ? "enabled" : "disabled", g_key[0], g_key[1],
		g_key[2], g_ramp[0], g_ramp[1], g_guard);
}

// ---------------------------------------------------------------- headset settings over the LAN
// Every 2 s (only once an app uses ALPHA_BLEND) ask "FPPQ1" by UDP broadcast on
// each network; headsets running the Frame Key panel answer with their key
// settings. Use the one streaming to this PC, else the one pinned in
// alphakey.ini (headset = <id>), else the only one. alphakey.ini's key and ramp
// are only used until a headset answers.
struct HeadsetInfo {
	std::string name;
	float key[3], tol, soft;
	bool streaming;
	ULONGLONG seen = 0, toYouAt = 0;
};
static SOCKET g_sock = INVALID_SOCKET;
static std::vector<ULONG> g_bcast; // network byte order
static ULONGLONG g_nextQuery, g_nextAdapters;
static std::map<std::string, HeadsetInfo> g_heads;
static std::string g_using;

static void refreshBroadcasts() {
	g_bcast.assign(1, INADDR_BROADCAST);
	ULONG size = 16384;
	std::vector<char> buf(size);
	auto *aa = (IP_ADAPTER_ADDRESSES *)buf.data();
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
		    aa, &size) != NO_ERROR)
		return;
	for (; aa; aa = aa->Next) {
		if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		for (auto *u = aa->FirstUnicastAddress; u; u = u->Next) {
			auto *sa = (sockaddr_in *)u->Address.lpSockaddr;
			ULONG mask = u->OnLinkPrefixLength ? htonl(~0u << (32 - u->OnLinkPrefixLength)) : 0;
			if (u->OnLinkPrefixLength && u->OnLinkPrefixLength < 31) g_bcast.push_back(sa->sin_addr.s_addr | ~mask);
		}
	}
}

static bool parseAnswer(const char *msg, std::string &id, HeadsetInfo &h, int &toyou) {
	if (strncmp(msg, "FPPA1\n", 6)) return false;
	char name[128] = "", idb[64] = "";
	toyou = 0;
	int st = 0;
	for (const char *l = msg; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : nullptr) {
		sscanf(l, "id=%63[^\n]", idb);
		sscanf(l, "name=%127[^\n]", name);
		sscanf(l, "key=%f %f %f", &h.key[0], &h.key[1], &h.key[2]);
		sscanf(l, "tolerance=%f", &h.tol);
		sscanf(l, "softness=%f", &h.soft);
		sscanf(l, "streaming=%d", &st);
		sscanf(l, "toyou=%d", &toyou);
	}
	id = idb;
	h.name = name;
	h.streaming = st != 0;
	return !id.empty();
}

static void lanPoll() {
	ULONGLONG now = GetTickCount64();
	if (g_sock == INVALID_SOCKET) {
		WSADATA wd;
		if (WSAStartup(MAKEWORD(2, 2), &wd)) return;
		g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (g_sock == INVALID_SOCKET) return;
		u_long nb = 1;
		ioctlsocket(g_sock, FIONBIO, &nb);
		BOOL on = TRUE;
		setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
	}
	if (now >= g_nextAdapters) refreshBroadcasts(), g_nextAdapters = now + 30000;
	if (now >= g_nextQuery) {
		g_nextQuery = now + 2000;
		for (ULONG b : g_bcast) {
			sockaddr_in to = {};
			to.sin_family = AF_INET;
			to.sin_port = htons(47911);
			to.sin_addr.s_addr = b;
			sendto(g_sock, "FPPQ1", 5, 0, (sockaddr *)&to, sizeof to);
		}
	}
	char buf[1024];
	int n;
	while ((n = recv(g_sock, buf, sizeof buf - 1, 0)) > 0) {
		buf[n] = 0;
		std::string id;
		HeadsetInfo h;
		int toyou;
		if (!parseAnswer(buf, id, h, toyou)) continue;
		HeadsetInfo &cur = g_heads[id];
		ULONGLONG toYouAt = toyou == 1 ? now : cur.toYouAt; // one answer per network; any of them may say so
		cur = h;
		cur.seen = now;
		cur.toYouAt = toYouAt;
	}
	// pick a headset among those heard from in the last 6 s
	const HeadsetInfo *pick = nullptr;
	std::string pickId;
	int alive = 0, streamingN = 0;
	const HeadsetInfo *onlyStreaming = nullptr, *only = nullptr;
	std::string onlyStreamingId, onlyId;
	for (auto &kv : g_heads) {
		const HeadsetInfo &h = kv.second;
		if (now - h.seen > 6000) continue;
		alive++, only = &h, onlyId = kv.first;
		if (h.streaming) streamingN++, onlyStreaming = &h, onlyStreamingId = kv.first;
		if (now - h.toYouAt < 6000 && !pick) pick = &h, pickId = kv.first;
	}
	if (!pick && g_pin != "auto" && g_heads.count(g_pin) && now - g_heads[g_pin].seen < 6000)
		pick = &g_heads[g_pin], pickId = g_pin;
	if (!pick && streamingN == 1) pick = onlyStreaming, pickId = onlyStreamingId;
	if (!pick && alive == 1) pick = only, pickId = onlyId;
	if (!pick) return;
	float ramp0 = pick->tol, ramp1 = pick->tol + (std::max)(pick->soft, 0.01f);
	bool changed = pickId != g_using || memcmp(g_key, pick->key, sizeof g_key) || ramp0 != g_ramp[0] || ramp1 != g_ramp[1];
	if (!changed) return;
	memcpy(g_key, pick->key, sizeof g_key);
	g_ramp[0] = ramp0, g_ramp[1] = ramp1;
	if (!g_guardFromIni) g_guard = ramp1 + 0.03f;
	if (pickId != g_using) logf("using headset %s (%s)", pick->name.c_str(), pickId.c_str());
	g_using = pickId;
	logf("key %.3f %.3f %.3f, ramp %.3f..%.3f (from the headset)", g_key[0], g_key[1], g_key[2], g_ramp[0], g_ramp[1]);
}

// ---------------------------------------------------------------- D3D11 keying pass
static const char *kShader = R"(
Texture2D<float4> src : register(t0);
cbuffer C : register(b0) {
	float4 key;   // rgb, sRGB-encoded
	float4 opt;   // premultiplied, srgbView, guard
	float4 ramp;  // start, end
}
float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
float2 cbcr(float3 c) { float y = dot(c, LUMA); return float2((c.b - y) / 1.8556, (c.r - y) / 1.5748); }
float3 rgbFrom(float y, float2 cc) {
	float r = y + 1.5748 * cc.y, b = y + 1.8556 * cc.x;
	return float3(r, (y - LUMA.r * r - LUMA.b * b) / LUMA.g, b);
}
float3 toSrgb(float3 c) { c = saturate(c); return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
float3 toLinear(float3 c) { c = saturate(c); return c < 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps(float4 pos : SV_Position) : SV_Target {
	float4 c = src.Load(int3(pos.xy, 0));
	float a = saturate(c.a);
	float3 col = opt.x > 0.5 ? (a > 1e-4 ? c.rgb / a : 0) : c.rgb;   // straight colour
	float3 g = opt.y > 0.5 ? toSrgb(col) : saturate(col);           // what ends up in the video
	float2 k = cbcr(key.rgb), d = cbcr(g) - k;
	float dist = length(d);
	float2 dir = dist > 1e-4 ? d / dist : -normalize(k + 1e-6);
	float3 o;
	if (a > 0.996) {
		// opaque: keep it out of the key's reach
		o = dist < opt.z ? saturate(rgbFrom(dot(g, LUMA), k + dir * opt.z)) : g;
	} else if (a < 0.004) {
		o = key.rgb;
	} else {
		// partial: chroma on the ramp toward the content's direction, luma blended
		float y = lerp(dot(key.rgb, LUMA), dot(g, LUMA), a);
		o = saturate(rgbFrom(y, k + dir * lerp(ramp.x, ramp.y, a)));
	}
	return float4(opt.y > 0.5 ? toLinear(o) : o, 1);
}
)";

static bool isSrgb(DXGI_FORMAT f) {
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
static bool hasAlpha(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
		return true;
	default:
		return false;
	}
}

struct D3D {
	ID3D11Device1 *dev = nullptr;
	ID3D11DeviceContext1 *ctx = nullptr;
	ID3DDeviceContextState *state = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11Buffer *cb = nullptr;
	bool ok = false;

	bool init(ID3D11Device *d) {
		if (FAILED(d->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev))) return false;
		dev->GetImmediateContext1(&ctx);
		D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
		if (FAILED(dev->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
			    &state)))
			return false;
		ID3DBlob *b = nullptr, *err = nullptr;
		if (FAILED(D3DCompile(kShader, strlen(kShader), "alphakey", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b, &err))) {
			logf("vs compile: %s", err ? (char *)err->GetBufferPointer() : "?");
			return false;
		}
		dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
		b->Release();
		if (FAILED(D3DCompile(kShader, strlen(kShader), "alphakey", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &b, &err))) {
			logf("ps compile: %s", err ? (char *)err->GetBufferPointer() : "?");
			return false;
		}
		dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
		b->Release();
		D3D11_BUFFER_DESC bd = {48, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
		dev->CreateBuffer(&bd, nullptr, &cb);
		ok = vs && ps && cb;
		return ok;
	}
	void release() {
		for (IUnknown *u : {(IUnknown *)cb, (IUnknown *)ps, (IUnknown *)vs, (IUnknown *)state, (IUnknown *)ctx,
			     (IUnknown *)dev})
			if (u) u->Release();
		*this = D3D();
	}
};

struct Region {
	XrRect2Di rect;
	uint32_t slice;
	bool unpremul;
};
struct Swapchain {
	XrSession session;
	DXGI_FORMAT viewFormat;
	std::vector<ID3D11Texture2D *> images;
	std::deque<uint32_t> acquired;
	std::vector<Region> regions; // where the last ALPHA_BLEND frame used this swapchain as the scene
	ID3D11Texture2D *scratch = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
};
struct Session {
	D3D d3d;
	bool alpha = false; // the app's last frame asked for ALPHA_BLEND
};

static std::mutex g_m;
static std::unordered_map<XrSession, Session> g_sessions;
static std::unordered_map<XrSwapchain, Swapchain> g_swapchains;

static bool g_dump = getenv("ALPHAKEY_DUMP") != nullptr;
static void dumpImage(D3D &d, ID3D11Texture2D *img, const D3D11_TEXTURE2D_DESC &td, const Region &r) {
	if (td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
		td.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { logf("dump: format %d not supported", td.Format); return; }
	D3D11_TEXTURE2D_DESC sd = td;
	sd.ArraySize = sd.MipLevels = 1;
	sd.BindFlags = sd.MiscFlags = 0;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *st = nullptr;
	if (FAILED(d.dev->CreateTexture2D(&sd, nullptr, &st))) return;
	d.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, img, D3D11CalcSubresource(0, r.slice, td.MipLevels), nullptr);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
		std::string path = g_dir + "alphakey_dump.ppm";
		if (FILE *f = fopen(path.c_str(), "wb")) {
			fprintf(f, "P6\n%u %u\n255\n", td.Width, td.Height);
			for (UINT y = 0; y < td.Height; y++) {
				const uint8_t *row = (const uint8_t *)m.pData + y * m.RowPitch;
				for (UINT x = 0; x < td.Width; x++) fwrite(row + x * 4, 1, 3, f);
			}
			fclose(f);
			logf("dumped %s", path.c_str());
		}
		d.ctx->Unmap(st, 0);
	}
	st->Release();
}

// Key one image just before the app releases it back to the runtime.
static void keyImage(Session &s, Swapchain &sc, uint32_t index) {
	D3D &d = s.d3d;
	ID3D11Texture2D *img = sc.images[index];
	D3D11_TEXTURE2D_DESC td;
	img->GetDesc(&td);
	if (td.SampleDesc.Count > 1) return;
	if (!sc.scratch) {
		// Views use the app's format, unless the runtime made a typed texture.
		if (td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
			td.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS && td.Format != DXGI_FORMAT_R10G10B10A2_TYPELESS)
			sc.viewFormat = td.Format;
		D3D11_TEXTURE2D_DESC sd = td;
		sd.ArraySize = 1;
		sd.MipLevels = 1;
		sd.SampleDesc = {1, 0};
		sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sd.MiscFlags = 0;
		sd.Usage = D3D11_USAGE_DEFAULT;
		sd.CPUAccessFlags = 0;
		if (FAILED(d.dev->CreateTexture2D(&sd, nullptr, &sc.scratch))) { logf("scratch texture failed"); return; }
		D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
		vd.Format = sc.viewFormat;
		vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		vd.Texture2D.MipLevels = 1;
		d.dev->CreateShaderResourceView(sc.scratch, &vd, &sc.srv);
	}
	ID3DDeviceContextState *prev = nullptr;
	d.ctx->SwapDeviceContextState(d.state, &prev);
	for (const Region &r : sc.regions) {
		if (r.slice >= td.ArraySize) continue;
		D3D11_BOX box = {(UINT)r.rect.offset.x, (UINT)r.rect.offset.y, 0,
			(UINT)(r.rect.offset.x + r.rect.extent.width), (UINT)(r.rect.offset.y + r.rect.extent.height), 1};
		d.ctx->CopySubresourceRegion(sc.scratch, 0, box.left, box.top, 0, img,
			D3D11CalcSubresource(0, r.slice, td.MipLevels), &box);
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = sc.viewFormat;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rd.Texture2DArray.FirstArraySlice = r.slice;
		rd.Texture2DArray.ArraySize = 1;
		ID3D11RenderTargetView *rtv = nullptr;
		if (FAILED(d.dev->CreateRenderTargetView(img, &rd, &rtv))) continue;
		float cbuf[12] = {g_key[0], g_key[1], g_key[2], 0, r.unpremul ? 0.f : 1.f, isSrgb(sc.viewFormat) ? 1.f : 0.f,
			g_guard, 0, g_ramp[0], g_ramp[1], 0, 0};
		d.ctx->UpdateSubresource(d.cb, 0, nullptr, cbuf, 0, 0);
		D3D11_VIEWPORT vp = {(float)box.left, (float)box.top, (float)(box.right - box.left),
			(float)(box.bottom - box.top), 0, 1};
		d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
		d.ctx->RSSetViewports(1, &vp);
		d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d.ctx->VSSetShader(d.vs, nullptr, 0);
		d.ctx->PSSetShader(d.ps, nullptr, 0);
		d.ctx->PSSetShaderResources(0, 1, &sc.srv);
		d.ctx->PSSetConstantBuffers(0, 1, &d.cb);
		d.ctx->Draw(3, 0);
		ID3D11ShaderResourceView *nosrv = nullptr;
		d.ctx->PSSetShaderResources(0, 1, &nosrv);
		d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
		rtv->Release();
	}
	d.ctx->SwapDeviceContextState(prev, nullptr);
	if (prev) prev->Release();
	if (g_dump && !sc.regions.empty()) { // ALPHAKEY_DUMP=1: save the first keyed view once, for checking
		g_dump = false;
		dumpImage(d, img, td, sc.regions[0]);
	}
}

// ---------------------------------------------------------------- OpenXR hooks
static PFN_xrGetInstanceProcAddr g_next;
#define NEXT(name) PFN_##name next_##name;
#define HOOKED(X) X(xrEnumerateEnvironmentBlendModes) X(xrCreateSession) X(xrDestroySession) X(xrCreateSwapchain) \
	X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrReleaseSwapchainImage) X(xrEndFrame)
HOOKED(NEXT)

static XrResult XRAPI_CALL EnumerateEnvironmentBlendModes(XrInstance inst, XrSystemId sys, XrViewConfigurationType vct,
	uint32_t cap, uint32_t *count, XrEnvironmentBlendMode *modes) {
	uint32_t n = 0;
	XrResult r = next_xrEnumerateEnvironmentBlendModes(inst, sys, vct, 0, &n, nullptr);
	if (XR_FAILED(r)) return r;
	std::vector<XrEnvironmentBlendMode> v(n);
	next_xrEnumerateEnvironmentBlendModes(inst, sys, vct, n, &n, v.data());
	bool have = false;
	for (auto m : v) have |= m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
	if (!have) v.push_back(XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
	*count = (uint32_t)v.size();
	if (cap == 0) return XR_SUCCESS;
	if (cap < v.size()) return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(modes, v.data(), v.size() * sizeof v[0]);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL CreateSession(XrInstance inst, const XrSessionCreateInfo *ci, XrSession *out) {
	XrResult r = next_xrCreateSession(inst, ci, out);
	if (XR_FAILED(r)) return r;
	const XrGraphicsBindingD3D11KHR *b = nullptr;
	for (auto *p = (const XrBaseInStructure *)ci->next; p; p = p->next)
		if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) b = (const XrGraphicsBindingD3D11KHR *)p;
	std::lock_guard<std::mutex> l(g_m);
	Session &s = g_sessions[*out];
	if (b && s.d3d.init(b->device)) logf("session %p: D3D11, keying ready", (void *)*out);
	else logf("session %p: not D3D11 (or init failed), transparent pixels will not be keyed", (void *)*out);
	return r;
}

static XrResult XRAPI_CALL DestroySession(XrSession s) {
	{
		std::lock_guard<std::mutex> l(g_m);
		for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
			if (it->second.session == s) {
				if (it->second.srv) it->second.srv->Release();
				if (it->second.scratch) it->second.scratch->Release();
				it = g_swapchains.erase(it);
			} else
				++it;
		g_sessions[s].d3d.release();
		g_sessions.erase(s);
	}
	return next_xrDestroySession(s);
}

static XrResult XRAPI_CALL CreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
	XrResult r = next_xrCreateSwapchain(s, ci, out);
	if (XR_FAILED(r)) return r;
	std::lock_guard<std::mutex> l(g_m);
	auto it = g_sessions.find(s);
	if (it == g_sessions.end() || !it->second.d3d.ok) return r;
	if (!(ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) || !hasAlpha((DXGI_FORMAT)ci->format)) return r;
	Swapchain &sc = g_swapchains[*out];
	sc.session = s;
	sc.viewFormat = (DXGI_FORMAT)ci->format;
	uint32_t n = 0;
	next_xrEnumerateSwapchainImages(*out, 0, &n, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	next_xrEnumerateSwapchainImages(*out, n, &n, (XrSwapchainImageBaseHeader *)imgs.data());
	for (auto &i : imgs) sc.images.push_back(i.texture);
	logf("swapchain %p: %ux%u x%u format %lld, %u images", (void *)*out, ci->width, ci->height, ci->arraySize,
		(long long)ci->format, n);
	return r;
}

static XrResult XRAPI_CALL DestroySwapchain(XrSwapchain h) {
	{
		std::lock_guard<std::mutex> l(g_m);
		auto it = g_swapchains.find(h);
		if (it != g_swapchains.end()) {
			if (it->second.srv) it->second.srv->Release();
			if (it->second.scratch) it->second.scratch->Release();
			g_swapchains.erase(it);
		}
	}
	return next_xrDestroySwapchain(h);
}

static XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *ai, uint32_t *index) {
	XrResult r = next_xrAcquireSwapchainImage(h, ai, index);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> l(g_m);
		auto it = g_swapchains.find(h);
		if (it != g_swapchains.end()) it->second.acquired.push_back(*index);
	}
	return r;
}

static XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *ri) {
	{
		std::lock_guard<std::mutex> l(g_m);
		auto it = g_swapchains.find(h);
		if (it != g_swapchains.end() && !it->second.acquired.empty()) {
			Swapchain &sc = it->second;
			uint32_t index = sc.acquired.front();
			sc.acquired.pop_front();
			Session &s = g_sessions[sc.session];
			if (g_enabled && s.alpha && s.d3d.ok && !sc.regions.empty() && index < sc.images.size())
				keyImage(s, sc, index);
		}
	}
	return next_xrReleaseSwapchainImage(h, ri);
}

// Remember where the scene layer's views live, so the next release of those
// images gets keyed, and submit as OPAQUE.
// alphakey.ini is reread when it changes (checked about once a second), so the
// PC app's colour and ramp apply without restarting the game.
static void reloadConfigIfChanged() {
	static ULONGLONG nextCheck = 0;
	static FILETIME seen = {};
	ULONGLONG now = GetTickCount64();
	if (now < nextCheck) return;
	nextCheck = now + 1000;
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA((g_dir + "alphakey.ini").c_str(), GetFileExInfoStandard, &fa)) return;
	if (CompareFileTime(&fa.ftLastWriteTime, &seen) == 0) return;
	bool first = seen.dwLowDateTime == 0 && seen.dwHighDateTime == 0;
	seen = fa.ftLastWriteTime;
	if (!first) loadConfig();
}

static XrResult XRAPI_CALL EndFrame(XrSession s, const XrFrameEndInfo *fi) {
	reloadConfigIfChanged();
	XrFrameEndInfo info = *fi;
	if (fi->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
		std::lock_guard<std::mutex> l(g_m);
		Session &sess = g_sessions[s];
		if (!sess.alpha) logf("session %p: app switched to ALPHA_BLEND", (void *)s);
		if (g_enabled) lanPoll();
		sess.alpha = true;
		for (auto &kv : g_swapchains)
			if (kv.second.session == s) kv.second.regions.clear();
		if (fi->layerCount && fi->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
			auto *p = (const XrCompositionLayerProjection *)fi->layers[0];
			bool unpremul = p->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
			for (uint32_t v = 0; v < p->viewCount; v++) {
				auto &si = p->views[v].subImage;
				auto it = g_swapchains.find(si.swapchain);
				if (it != g_swapchains.end()) it->second.regions.push_back({si.imageRect, si.imageArrayIndex, unpremul});
			}
		}
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	} else {
		std::lock_guard<std::mutex> l(g_m);
		Session &sess = g_sessions[s];
		if (sess.alpha) logf("session %p: app left ALPHA_BLEND", (void *)s);
		sess.alpha = false;
	}
	return next_xrEndFrame(s, &info);
}

static XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance inst, const char *name, PFN_xrVoidFunction *fn) {
	if (!g_enabled && strcmp(name, "xrGetInstanceProcAddr")) return g_next(inst, name, fn);
	static const struct { const char *name; PFN_xrVoidFunction fn; } hooks[] = {
		{"xrGetInstanceProcAddr", (PFN_xrVoidFunction)GetInstanceProcAddr},
		{"xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction)EnumerateEnvironmentBlendModes},
		{"xrCreateSession", (PFN_xrVoidFunction)CreateSession},
		{"xrDestroySession", (PFN_xrVoidFunction)DestroySession},
		{"xrCreateSwapchain", (PFN_xrVoidFunction)CreateSwapchain},
		{"xrDestroySwapchain", (PFN_xrVoidFunction)DestroySwapchain},
		{"xrAcquireSwapchainImage", (PFN_xrVoidFunction)AcquireSwapchainImage},
		{"xrReleaseSwapchainImage", (PFN_xrVoidFunction)ReleaseSwapchainImage},
		{"xrEndFrame", (PFN_xrVoidFunction)EndFrame},
	};
	for (auto &h : hooks)
		if (!strcmp(name, h.name)) { *fn = h.fn; return XR_SUCCESS; }
	return g_next(inst, name, fn);
}

static XrResult XRAPI_CALL CreateApiLayerInstance(const XrInstanceCreateInfo *ci, const XrApiLayerCreateInfo *li,
	XrInstance *out) {
	loadConfig();
	g_next = li->nextInfo->nextGetInstanceProcAddr;
	XrApiLayerCreateInfo next = *li;
	next.nextInfo = li->nextInfo->next;
	XrResult r = li->nextInfo->nextCreateApiLayerInstance(ci, &next, out);
	if (XR_FAILED(r)) return r;
#define GET(name) g_next(*out, #name, (PFN_xrVoidFunction *)&next_##name);
	HOOKED(GET)
	logf("instance for %s", ci->applicationInfo.applicationName);
	return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
	const XrNegotiateLoaderInfo *li, const char *layerName, XrNegotiateApiLayerRequest *req) {
	if (!li || !req || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
		li->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
		li->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
		return XR_ERROR_INITIALIZATION_FAILED;
	req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
	req->layerApiVersion = XR_CURRENT_API_VERSION;
	req->getInstanceProcAddr = GetInstanceProcAddr;
	req->createApiLayerInstance = CreateApiLayerInstance;
	return XR_SUCCESS;
}
