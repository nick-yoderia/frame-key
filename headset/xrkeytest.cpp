// xrkeytest: a native OpenXR app for testing keying on the Frame itself.
// Each eye is filled with the key colour, with a grey square in the middle,
// like a green-screen video. Opaque blend mode, as video players use.
//   xrkeytest [RRGGBB] [seconds] [alpha]   default 00ff00 for 30 s, opaque
// "alpha" submits with ALPHA_BLEND instead (everything opaque all the same).
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define XRC(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static uint32_t memType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(1);
}

int main(int argc, char **argv) {
	unsigned rgb = argc > 1 ? strtoul(argv[1], nullptr, 16) : 0x00ff00;
	double seconds = argc > 2 ? atof(argv[2]) : 30;
	bool alphaBlend = argc > 3 && !strcmp(argv[3], "alpha");
	uint8_t key[4] = {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), 255};

	const char *exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ici.applicationInfo.applicationName, "xrkeytest");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = exts;
	XrInstance inst;
	XRC(xrCreateInstance(&ici, &inst));
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	XRC(xrGetSystem(inst, &sgi, &sys));

	PFN_xrGetVulkanGraphicsRequirements2KHR getReq;
	PFN_xrCreateVulkanInstanceKHR createVkInst;
	PFN_xrGetVulkanGraphicsDevice2KHR getVkPd;
	PFN_xrCreateVulkanDeviceKHR createVkDev;
	xrGetInstanceProcAddr(inst, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&getReq);
	xrGetInstanceProcAddr(inst, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&createVkInst);
	xrGetInstanceProcAddr(inst, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&getVkPd);
	xrGetInstanceProcAddr(inst, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&createVkDev);
	XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	XRC(getReq(inst, sys, &req));

	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &ai;
	XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xvi.systemId = sys;
	xvi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xvi.vulkanCreateInfo = &vici;
	VkInstance vki;
	VkResult vr;
	XRC(createVkInst(inst, &xvi, &vki, &vr));
	VKC(vr);
	XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = sys;
	gdi.vulkanInstance = vki;
	VkPhysicalDevice pd;
	XRC(getVkPd(inst, &gdi, &pd));
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
	uint32_t qfi = 0;
	while (qfi < nq && !(qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi++;
	float prio = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	XrVulkanDeviceCreateInfoKHR xvd{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xvd.systemId = sys;
	xvd.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xvd.vulkanPhysicalDevice = pd;
	xvd.vulkanCreateInfo = &dci;
	VkDevice dev;
	XRC(createVkDev(inst, &xvd, &dev, &vr));
	VKC(vr);
	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);

	XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	gb.instance = vki;
	gb.physicalDevice = pd;
	gb.device = dev;
	gb.queueFamilyIndex = qfi;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &gb;
	sci.systemId = sys;
	XrSession session;
	XRC(xrCreateSession(inst, &sci, &session));
	XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1;
	XrSpace space;
	XRC(xrCreateReferenceSpace(session, &rsci, &space));

	uint32_t nv = 0;
	XrViewConfigurationView vcv[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XRC(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv));
	uint32_t w = vcv[0].recommendedImageRectWidth, h = vcv[0].recommendedImageRectHeight;

	// one swapchain, two array layers
	XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	swci.format = VK_FORMAT_R8G8B8A8_SRGB;
	swci.sampleCount = 1;
	swci.width = w;
	swci.height = h;
	swci.faceCount = 1;
	swci.arraySize = 2;
	swci.mipCount = 1;
	XrSwapchain sc;
	XRC(xrCreateSwapchain(session, &swci, &sc));
	uint32_t ni = 0;
	XRC(xrEnumerateSwapchainImages(sc, 0, &ni, nullptr));
	std::vector<XrSwapchainImageVulkan2KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	XRC(xrEnumerateSwapchainImages(sc, ni, &ni, (XrSwapchainImageBaseHeader *)imgs.data()));

	// staging buffer: the whole eye in the key colour, a grey square in the middle
	VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer buf;
	VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = memType(pd, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory mem;
	VKC(vkAllocateMemory(dev, &mai, nullptr, &mem));
	VKC(vkBindBufferMemory(dev, buf, mem, 0));
	uint8_t *px;
	VKC(vkMapMemory(dev, mem, 0, bytes, 0, (void **)&px));
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			bool grey = x > w * 3 / 8 && x < w * 5 / 8 && y > h * 3 / 8 && y < h * 5 / 8;
			uint8_t *p = px + (size_t(y) * w + x) * 4;
			if (grey) p[0] = p[1] = p[2] = 128, p[3] = 255;
			else memcpy(p, key, 4);
		}

	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = qfi;
	VkCommandPool pool;
	VKC(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VKC(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VKC(vkCreateFence(dev, &fci, nullptr, &fence));

	auto start = std::chrono::steady_clock::now();
	bool running = false, quit = false;
	while (!quit) {
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				auto st = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (st == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
					sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					XRC(xrBeginSession(session, &sbi));
					running = true;
				} else if (st == XR_SESSION_STATE_STOPPING) {
					xrEndSession(session);
					running = false;
				} else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
					quit = true;
				}
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) {
			if (running) xrRequestExitSession(session);
			else quit = true;
		}
		if (!running) {
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, nullptr);
			continue;
		}

		XrFrameState fs{XR_TYPE_FRAME_STATE};
		XRC(xrWaitFrame(session, nullptr, &fs));
		XRC(xrBeginFrame(session, nullptr));
		XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = fs.predictedDisplayTime;
		vli.space = space;
		XrViewState vs{XR_TYPE_VIEW_STATE};
		XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		XRC(xrLocateViews(session, &vli, &vs, 2, &nv, views));

		uint32_t idx;
		XRC(xrAcquireSwapchainImage(sc, nullptr, &idx));
		XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wi.timeout = XR_INFINITE_DURATION;
		XRC(xrWaitSwapchainImage(sc, &wi));
		VkImage img = imgs[idx].image;
		VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		VKC(vkBeginCommandBuffer(cb, &cbbi));
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
			0, nullptr, 1, &b);
		VkBufferImageCopy cp{};
		cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
		cp.imageExtent = {w, h, 1};
		VkBufferImageCopy cps[2] = {cp, cp};
		cps[0].imageSubresource.layerCount = cps[1].imageSubresource.layerCount = 1;
		cps[1].imageSubresource.baseArrayLayer = 1;
		vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cps);
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
			0, nullptr, 1, &b);
		VKC(vkEndCommandBuffer(cb));
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		VKC(vkQueueSubmit(queue, 1, &si, fence));
		VKC(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		VKC(vkResetFences(dev, 1, &fence));
		XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		XRC(xrReleaseSwapchainImage(sc, &ri));

		XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (int e = 0; e < 2; e++) {
			pv[e].pose = views[e].pose;
			pv[e].fov = views[e].fov;
			pv[e].subImage.swapchain = sc;
			pv[e].subImage.imageRect = {{0, 0}, {int32_t(w), int32_t(h)}};
			pv[e].subImage.imageArrayIndex = e;
		}
		XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.space = space;
		proj.viewCount = 2;
		proj.views = pv;
		const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = alphaBlend ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = fs.shouldRender ? 1 : 0;
		fei.layers = layers;
		XRC(xrEndFrame(session, &fei));
	}
	vkDeviceWaitIdle(dev);
	xrDestroySession(session);
	xrDestroyInstance(inst);
	return 0;
}
