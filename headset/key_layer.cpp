// VK_LAYER_FRAME_KEYER: Vulkan layer for vrcompositor on the Steam Frame.
//
// The Steam Link stream reaches vrcompositor as NV12 video, which has no alpha,
// and the shader that draws it (distort_ps_achromatic_se2) writes alpha = 1.
// Native Frame apps are drawn by distort_ps_achromatic_alphaed (or _achromatic)
// plus fsharp_vr_ps (the sharp circle where you look). When
// ~/.local/share/frame-keyer/se2.spv (alphaed.spv, ...) exists, this layer hands
// the compositor that file instead of the stock shader: a copy patched by
// gen_key.py that writes alpha < 1 where the picture shows the key colour.
// With camera/forceSceneAlphaBlending on, the compositor then blends its own
// passthrough in wherever scene alpha < 1. Camera pixels never pass through us.
//
// The key colour and sliders aren't in those shaders: they read them from push
// constants. We give every pipeline layout of the compositor (none of its own
// shaders use push constants) a 64-byte fragment range, and whenever a keyed
// pipeline is bound we push the settings from ~/.local/share/frame-keyer/params
// (written by gen_key.py, checked for changes a few times a second). So a
// setting changes without `vrcmd --reload-shaders`, which rebuilds every
// pipeline of the compositor.
//
// Loads only into vrcompositor, and only while ~/.config/frame-keyer/enable
// exists. Otherwise every call goes straight to the next layer.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

static const char *kDir = "/opt/steamvr/resources/shaders/vulkan/";
// Shaders we may swap: stock file in kDir -> our copy in ~/.local/share/frame-keyer/
static const struct Target {
	const char *stock, *ours;
} kTargets[] = {
	{"distort_ps_achromatic_se2.spv", "se2"},       // Steam Link stream
	{"distort_ps_achromatic_alphaed.spv", "alphaed"}, // native apps
	{"distort_ps_achromatic.spv", "achromatic"},     // native apps without alpha
	{"fsharp_vr_ps.spv", "fsharp"},                   // native apps, sharp eye-tracked circle
};

static std::string home(const char *rel) {
	const char *h = getenv("HOME");
	return std::string(h ? h : "/tmp") + rel;
}
static bool exists(const std::string &p) {
	FILE *f = fopen(p.c_str(), "rb");
	if (f) fclose(f);
	return f != nullptr;
}
static std::vector<char> readFile(const std::string &p) {
	std::vector<char> v;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return v;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0) {
		v.resize(n);
		if (fread(v.data(), 1, n, f) != (size_t)n) v.clear();
	}
	fclose(f);
	return v;
}

static FILE *g_log;
static void logf(const char *fmt, ...) {
	if (!g_log) return;
	time_t t = time(nullptr);
	char ts[32];
	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
	fprintf(g_log, "%s ", ts);
	va_list a;
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

// Active = this process is vrcompositor and the user opted in.
static bool active() {
	static int a = -1;
	if (a < 0) {
		char comm[64] = {};
		if (FILE *f = fopen("/proc/self/comm", "r")) {
			if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
			fclose(f);
		}
		comm[strcspn(comm, "\n")] = 0;
		a = !strcmp(comm, "vrcompositor") && exists(home("/.config/frame-keyer/enable"));
		if (a) {
			g_log = fopen(home("/.local/share/frame-keyer/layer.log").c_str(), "w");
			logf("loaded into %s (pid %d)", comm, (int)getpid());
		}
	}
	return a;
}

struct Instance {
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkDestroyInstance DestroyInstance;
};
struct Device {
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkCmdBindPipeline CmdBindPipeline;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkCmdPushConstants CmdPushConstants;
	PFN_vkDestroyShaderModule DestroyShaderModule;
};
static std::mutex g_m;
static std::unordered_map<void *, Instance> g_inst;
static std::unordered_map<void *, Device> g_dev;
static void *key(const void *h) { return *(void **)h; } // dispatch table pointer

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
	VkInstance *out) {
	auto *li = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
		li = (VkLayerInstanceCreateInfo *)li->pNext;
	if (!li) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	li->u.pLayerInfo = li->u.pLayerInfo->pNext;
	auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
	VkResult r = create(ci, a, out);
	if (r != VK_SUCCESS) return r;
	std::lock_guard<std::mutex> l(g_m);
	g_inst[key(*out)] = {gipa, (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance")};
	return r;
}
static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance i, const VkAllocationCallbacks *a) {
	PFN_vkDestroyInstance d;
	{
		std::lock_guard<std::mutex> l(g_m);
		d = g_inst[key(i)].DestroyInstance;
		g_inst.erase(key(i));
	}
	d(i, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
	const VkAllocationCallbacks *a, VkDevice *out) {
	auto *li = (VkLayerDeviceCreateInfo *)ci->pNext;
	while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
		li = (VkLayerDeviceCreateInfo *)li->pNext;
	if (!li) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	li->u.pLayerInfo = li->u.pLayerInfo->pNext;
	auto create = (PFN_vkCreateDevice)gipa(nullptr, "vkCreateDevice");
	VkResult r = create(pd, ci, a, out);
	if (r != VK_SUCCESS) return r;
	std::lock_guard<std::mutex> l(g_m);
	g_dev[key(*out)] = {gdpa, (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice"),
		(PFN_vkCreateShaderModule)gdpa(*out, "vkCreateShaderModule"),
		(PFN_vkCreateGraphicsPipelines)gdpa(*out, "vkCreateGraphicsPipelines"),
		(PFN_vkCreateComputePipelines)gdpa(*out, "vkCreateComputePipelines"),
		(PFN_vkCmdBindPipeline)gdpa(*out, "vkCmdBindPipeline"),
		(PFN_vkCreatePipelineLayout)gdpa(*out, "vkCreatePipelineLayout"),
		(PFN_vkCmdPushConstants)gdpa(*out, "vkCmdPushConstants"),
		(PFN_vkDestroyShaderModule)gdpa(*out, "vkDestroyShaderModule")};
	return r;
}
static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a) {
	PFN_vkDestroyDevice d;
	{
		std::lock_guard<std::mutex> l(g_m);
		d = g_dev[key(dev)].DestroyDevice;
		g_dev.erase(key(dev));
	}
	d(dev, a);
}

// Trace (~/.config/frame-keyer/trace exists): name every shader module after
// its file in SteamVR's shader folder and log, every 5 s, how often each
// pipeline was bound. Shows which shader draws what.
static bool tracing() {
	static int t = -1;
	if (t < 0) t = active() && exists(home("/.config/frame-keyer/trace"));
	return t;
}
static uint64_t fnv(const void *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char *)p)[i]) * 1099511628211ull;
	return h;
}
static std::unordered_map<uint64_t, std::string> g_names;  // content hash -> file name
static std::unordered_map<uint64_t, std::string> g_modules; // VkShaderModule -> name
static std::unordered_map<uint64_t, std::string> g_pipes;   // VkPipeline -> "vs+ps"
static std::map<std::string, long> g_binds;
static time_t g_lastDump;
static std::string nameOf(const void *code, size_t n) {
	if (g_names.empty())
		if (DIR *d = opendir(kDir)) {
			while (dirent *e = readdir(d)) {
				std::vector<char> v = readFile(std::string(kDir) + e->d_name);
				if (!v.empty()) g_names[fnv(v.data(), v.size())] = e->d_name;
			}
			closedir(d);
		}
	auto it = g_names.find(fnv(code, n));
	if (it != g_names.end()) return it->second;
	char b[40];
	snprintf(b, sizeof b, "?%zu", n);
	return b;
}
static std::string stageName(const VkPipelineShaderStageCreateInfo &st) {
	auto it = g_modules.find((uint64_t)st.module);
	if (it != g_modules.end()) return it->second;
	for (auto *p = (const VkBaseInStructure *)st.pNext; p; p = p->pNext)
		if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
			auto *mi = (const VkShaderModuleCreateInfo *)p;
			return nameOf(mi->pCode, mi->codeSize);
		}
	return "?";
}
// The stock shaders, read once from disk; they are recognised by their exact bytes.
static const std::vector<char> &stockCode(size_t t) {
	static std::vector<char> stock[sizeof kTargets / sizeof *kTargets];
	static std::once_flag once;
	std::call_once(once, [] {
		for (size_t t = 0; t < sizeof kTargets / sizeof *kTargets; t++)
			stock[t] = readFile(std::string(kDir) + kTargets[t].stock);
	});
	return stock[t];
}

// Settings as push constants. The params file is a 16-byte header ("FKP1"),
// then one block for the stream and one for native apps (gen_key.py PC).
static const uint32_t kPcBytes = 64;
static std::unordered_map<uint64_t, size_t> g_keyedMods; // keyed VkShaderModule -> kTargets index
static std::unordered_set<uint64_t> g_pcLayouts;          // layouts we gave our range
struct KeyedPipe {
	VkPipelineLayout layout;
	int block; // 0 stream, 1 native apps
};
static std::unordered_map<uint64_t, KeyedPipe> g_keyedPipes;
static char g_params[16 + 2 * kPcBytes];
// Without a params file, settings that key nothing (gen_key.py HOLD).
static void holdParams() {
	static const float hold[] = {0, 0, 1, 0, -1000, 1, -1001, -1000, 0};
	memset(g_params, 0, sizeof g_params);
	memcpy(g_params, "FKP1", 4);
	for (int b = 0; b < 2; b++) memcpy(g_params + 16 + kPcBytes * b, hold, sizeof hold);
}
// Current settings; the file is checked at most every 200 ms. Call with g_m held.
static const char *params() {
	static double next = 0;
	static struct stat seen {};
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	double now = ts.tv_sec + ts.tv_nsec * 1e-9;
	if (next == 0) holdParams();
	if (now < next) return g_params;
	next = now + 0.2;
	std::string p = home("/.local/share/frame-keyer/params");
	struct stat st {};
	if (stat(p.c_str(), &st) != 0) {
		if (seen.st_ino) logf("params: none, keying nothing");
		if (seen.st_ino) holdParams();
		seen = {};
		return g_params;
	}
	if (st.st_ino == seen.st_ino && st.st_size == seen.st_size && st.st_mtim.tv_sec == seen.st_mtim.tv_sec &&
		st.st_mtim.tv_nsec == seen.st_mtim.tv_nsec)
		return g_params;
	seen = st;
	std::vector<char> v = readFile(p);
	if (v.size() >= sizeof g_params && !memcmp(v.data(), "FKP1", 4)) {
		memcpy(g_params, v.data(), sizeof g_params);
		logf("params updated");
	} else {
		logf("params: not a params file, keeping the last settings");
	}
	return g_params;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *ci,
	const VkAllocationCallbacks *a, VkPipelineLayout *out) {
	PFN_vkCreatePipelineLayout next;
	{
		std::lock_guard<std::mutex> l(g_m);
		next = g_dev[key(dev)].CreatePipelineLayout;
	}
	bool add = active() && ci->pushConstantRangeCount == 0;
	static const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPcBytes};
	VkPipelineLayoutCreateInfo li = *ci;
	if (add) li.pushConstantRangeCount = 1, li.pPushConstantRanges = &range;
	VkResult r = next(dev, &li, a, out);
	if (r == VK_SUCCESS && active()) {
		std::lock_guard<std::mutex> l(g_m);
		if (add) g_pcLayouts.insert((uint64_t)*out);
		else g_pcLayouts.erase((uint64_t)*out);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice dev, VkPipelineCache c, uint32_t n,
	const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
	Device d;
	std::vector<int> block(n, -1);
	std::vector<std::pair<uint32_t, uint32_t>> fallback; // (pipeline, stage) needing the stock shader
	{
		std::lock_guard<std::mutex> l(g_m);
		d = g_dev[key(dev)];
		for (uint32_t i = 0; i < n; i++)
			for (uint32_t s = 0; s < ci[i].stageCount; s++) {
				auto it = g_keyedMods.find((uint64_t)ci[i].pStages[s].module);
				if (it == g_keyedMods.end()) continue;
				if (g_pcLayouts.count((uint64_t)ci[i].layout)) block[i] = it->second == 0 ? 0 : 1;
				else fallback.push_back({i, s});
			}
	}
	// A keyed shader needs our push constant range. A layout that came with
	// ranges of its own didn't get it, so that pipeline gets the stock shader.
	std::vector<VkGraphicsPipelineCreateInfo> cis;
	std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(n);
	std::vector<VkShaderModule> temp;
	if (!fallback.empty()) {
		cis.assign(ci, ci + n);
		for (auto [i, s] : fallback) {
			size_t t;
			{
				std::lock_guard<std::mutex> l(g_m);
				t = g_keyedMods[(uint64_t)ci[i].pStages[s].module];
			}
			const std::vector<char> &st = stockCode(t);
			VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			mci.codeSize = st.size();
			mci.pCode = (const uint32_t *)st.data();
			VkShaderModule m;
			if (st.empty() || d.CreateShaderModule(dev, &mci, a, &m) != VK_SUCCESS) continue;
			temp.push_back(m);
			if (stages[i].empty()) stages[i].assign(ci[i].pStages, ci[i].pStages + ci[i].stageCount);
			stages[i][s].module = m;
			cis[i].pStages = stages[i].data();
			logf("%s: pipeline layout has its own push constants, using the stock shader", kTargets[t].ours);
		}
		ci = cis.data();
	}
	VkResult r = d.CreateGraphicsPipelines(dev, c, n, ci, a, out);
	for (VkShaderModule m : temp) d.DestroyShaderModule(dev, m, a);
	std::lock_guard<std::mutex> l(g_m);
	for (uint32_t i = 0; i < n; i++) {
		if (!out[i]) continue;
		if (block[i] >= 0) g_keyedPipes[(uint64_t)out[i]] = {ci[i].layout, block[i]};
		else g_keyedPipes.erase((uint64_t)out[i]);
	}
	if (!tracing()) return r;
	for (uint32_t i = 0; i < n; i++) {
		std::string vs, ps;
		for (uint32_t s = 0; s < ci[i].stageCount; s++) {
			if (ci[i].pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = stageName(ci[i].pStages[s]);
			if (ci[i].pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) ps = stageName(ci[i].pStages[s]);
		}
		if (out[i]) g_pipes[(uint64_t)out[i]] = vs + "+" + ps;
	}
	return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice dev, VkPipelineCache c, uint32_t n,
	const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
	PFN_vkCreateComputePipelines next;
	{
		std::lock_guard<std::mutex> l(g_m);
		next = g_dev[key(dev)].CreateComputePipelines;
	}
	VkResult r = next(dev, c, n, ci, a, out);
	if (!tracing()) return r;
	std::lock_guard<std::mutex> l(g_m);
	for (uint32_t i = 0; i < n; i++)
		if (out[i]) g_pipes[(uint64_t)out[i]] = "cs:" + stageName(ci[i].stage);
	return r;
}
static VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
	PFN_vkCmdBindPipeline next;
	PFN_vkCmdPushConstants push = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	char data[kPcBytes];
	{
		std::lock_guard<std::mutex> l(g_m);
		Device &d = g_dev[key(cb)];
		next = d.CmdBindPipeline;
		auto kp = bp == VK_PIPELINE_BIND_POINT_GRAPHICS ? g_keyedPipes.find((uint64_t)p) : g_keyedPipes.end();
		if (kp != g_keyedPipes.end()) {
			push = d.CmdPushConstants;
			layout = kp->second.layout;
			memcpy(data, params() + 16 + kPcBytes * kp->second.block, kPcBytes);
		}
		if (tracing()) {
			auto it = g_pipes.find((uint64_t)p);
			g_binds[it == g_pipes.end() ? "?" : it->second]++;
			time_t now = time(nullptr);
			if (now - g_lastDump >= 5) {
				g_lastDump = now;
				std::string line;
				for (auto &b : g_binds) line += "\n  " + std::to_string(b.second) + " " + b.first;
				logf("binds in the last 5 s:%s", line.c_str());
				g_binds.clear();
			}
		}
	}
	next(cb, bp, p);
	if (push) push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPcBytes, data);
}

// Sets *keyed to the kTargets index when it handed over our copy, else -1.
static VkResult swap(PFN_vkCreateShaderModule next, VkDevice dev, const VkShaderModuleCreateInfo *ci,
	const VkAllocationCallbacks *a, VkShaderModule *out, int *keyed) {
	*keyed = -1;
	for (size_t t = 0; t < sizeof kTargets / sizeof *kTargets; t++) {
		const std::vector<char> &st = stockCode(t);
		if (st.empty() || ci->codeSize != st.size() || memcmp(ci->pCode, st.data(), st.size())) continue;
		const char *name = kTargets[t].ours;
		// Re-read on every creation so `vrcmd --reload-shaders` picks up changes.
		std::vector<char> repl = readFile(home("/.local/share/frame-keyer/") + name + ".spv");
		if (repl.size() < 20 || repl.size() % 4 || *(const uint32_t *)repl.data() != 0x07230203) {
			logf("%s: stock%s", name, repl.empty() ? "" : " (override file is not SPIR-V)");
			break;
		}
		VkShaderModuleCreateInfo mci = *ci;
		mci.pCode = (const uint32_t *)repl.data();
		mci.codeSize = repl.size();
		VkResult r = next(dev, &mci, a, out);
		if (r == VK_SUCCESS) {
			logf("%s: keyed (%zu bytes)", name, repl.size());
			*keyed = (int)t;
			return r;
		}
		logf("%s: keyed shader rejected (%d), using stock", name, r);
		break;
	}
	return next(dev, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci,
	const VkAllocationCallbacks *a, VkShaderModule *out) {
	PFN_vkCreateShaderModule next;
	{
		std::lock_guard<std::mutex> l(g_m);
		next = g_dev[key(dev)].CreateShaderModule;
	}
	if (!active()) return next(dev, ci, a, out);
	int keyed;
	VkResult r = swap(next, dev, ci, a, out, &keyed);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> l(g_m);
		if (keyed >= 0) g_keyedMods[(uint64_t)*out] = keyed;
		else g_keyedMods.erase((uint64_t)*out);
		if (tracing()) g_modules[(uint64_t)*out] = nameOf(ci->pCode, ci->codeSize);
	}
	return r;
}

static PFN_vkVoidFunction ours(const char *n, bool device) {
	if (!strcmp(n, "vkCreateDevice") && !device) return (PFN_vkVoidFunction)CreateDevice;
	if (!strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
	if (!strcmp(n, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
	if (!strcmp(n, "vkCreatePipelineLayout")) return (PFN_vkVoidFunction)CreatePipelineLayout;
	if (!strcmp(n, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)CreateGraphicsPipelines;
	if (!strcmp(n, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)CreateComputePipelines;
	if (!strcmp(n, "vkCmdBindPipeline")) return (PFN_vkVoidFunction)CmdBindPipeline;
	return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char *n) {
	if (PFN_vkVoidFunction f = ours(n, true)) return f;
	std::lock_guard<std::mutex> l(g_m);
	return g_dev[key(dev)].gdpa(dev, n);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char *n) {
	if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)GetInstanceProcAddr;
	if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
	if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
	if (!strcmp(n, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
	if (PFN_vkVoidFunction f = ours(n, false)) return f;
	if (!inst) return nullptr;
	std::lock_guard<std::mutex> l(g_m);
	return g_inst[key(inst)].gipa(inst, n);
}

// Outside vrcompositor the layer still has to forward, so the loader sees a
// working layer; it just never touches anything.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL keyer_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
	if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
	if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = nullptr;
	active();
	return VK_SUCCESS;
}
