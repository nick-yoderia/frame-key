#include "vkshare.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using namespace vr;

// SteamVR returns required extensions as one space-separated string.
static std::vector<std::string> splitExtensions(const char *s) {
	std::vector<std::string> out;
	std::istringstream ss(s);
	std::string e;
	while (ss >> e) out.push_back(e);
	return out;
}

static std::vector<const char *> cstrs(const std::vector<std::string> &v) {
	std::vector<const char *> out;
	for (const std::string &s : v) out.push_back(s.c_str());
	return out;
}

#define VKCHECK(expr)                                                         \
	do {                                                                      \
		VkResult r_ = (expr);                                                 \
		if (r_ != VK_SUCCESS) {                                               \
			fprintf(stderr, "[vulkan] %s failed: %d\n", #expr, (int)r_);       \
			return false;                                                     \
		}                                                                     \
	} while (0)

bool VkShare::init() {
	char buf[4096] = {};
	VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof buf);
	std::vector<std::string> instExt = splitExtensions(buf);
	auto instExtC = cstrs(instExt);

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "frame-key";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)instExtC.size();
	ici.ppEnabledExtensionNames = instExtC.data();
	VKCHECK(vkCreateInstance(&ici, nullptr, &m_instance));

	uint64_t hmdDevice = 0;
	VRSystem()->GetOutputDevice(&hmdDevice, TextureType_Vulkan, m_instance);
	m_phys = (VkPhysicalDevice)hmdDevice;
	if (!m_phys) {
		uint32_t n = 1;
		vkEnumeratePhysicalDevices(m_instance, &n, &m_phys);
	}
	if (!m_phys) { fprintf(stderr, "[vulkan] no physical device\n"); return false; }

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &nq, qf.data());
	m_queueFamily = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { m_queueFamily = i; break; }
	if (m_queueFamily == UINT32_MAX) { fprintf(stderr, "[vulkan] no graphics queue\n"); return false; }

	memset(buf, 0, sizeof buf);
	VRCompositor()->GetVulkanDeviceExtensionsRequired(m_phys, buf, sizeof buf);
	std::vector<std::string> devExt = splitExtensions(buf);
	auto devExtC = cstrs(devExt);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = m_queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)devExtC.size();
	dci.ppEnabledExtensionNames = devExtC.data();
	VKCHECK(vkCreateDevice(m_phys, &dci, nullptr, &m_device));
	vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = m_queueFamily;
	VKCHECK(vkCreateCommandPool(m_device, &pci, nullptr, &m_pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = m_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VKCHECK(vkAllocateCommandBuffers(m_device, &cai, &m_cmd));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VKCHECK(vkCreateFence(m_device, &fci, nullptr, &m_fence));

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(m_phys, &props);
	printf("[vulkan] %s, %zu instance + %zu device extensions from SteamVR\n", props.deviceName, instExt.size(), devExt.size());
	return true;
}

void VkShare::shutdown() {
	if (!m_device) return;
	vkDeviceWaitIdle(m_device);
	vkDestroyFence(m_device, m_fence, nullptr);
	vkDestroyCommandPool(m_device, m_pool, nullptr);
	vkDestroyDevice(m_device, nullptr);
	vkDestroyInstance(m_instance, nullptr);
	m_device = VK_NULL_HANDLE;
	m_instance = VK_NULL_HANDLE;
}

uint32_t VkShare::findMemory(uint32_t typeBits, VkMemoryPropertyFlags props) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
	return UINT32_MAX;
}

bool VkShare::createTexture(uint32_t w, uint32_t h, VkTexture &t) {
	t.width = w;
	t.height = h;
	t.everUploaded = false;

	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_SRGB;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK(vkCreateImage(m_device, &ii, nullptr, &t.image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m_device, t.image, &mr);
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKCHECK(vkAllocateMemory(m_device, &mai, nullptr, &t.memory));
	VKCHECK(vkBindImageMemory(m_device, t.image, t.memory, 0));

	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = (VkDeviceSize)w * h * 4;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VKCHECK(vkCreateBuffer(m_device, &bi, nullptr, &t.staging));
	vkGetBufferMemoryRequirements(m_device, t.staging, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKCHECK(vkAllocateMemory(m_device, &mai, nullptr, &t.stagingMemory));
	VKCHECK(vkBindBufferMemory(m_device, t.staging, t.stagingMemory, 0));
	VKCHECK(vkMapMemory(m_device, t.stagingMemory, 0, VK_WHOLE_SIZE, 0, &t.mapped));
	return true;
}

void VkShare::destroyTexture(VkTexture &t) {
	if (!m_device) return;
	vkDeviceWaitIdle(m_device);
	if (t.mapped) vkUnmapMemory(m_device, t.stagingMemory);
	if (t.staging) vkDestroyBuffer(m_device, t.staging, nullptr);
	if (t.stagingMemory) vkFreeMemory(m_device, t.stagingMemory, nullptr);
	if (t.image) vkDestroyImage(m_device, t.image, nullptr);
	if (t.memory) vkFreeMemory(m_device, t.memory, nullptr);
	t = VkTexture();
}

bool VkShare::upload(VkTexture &t, const uint8_t *rgba) {
	memcpy(t.mapped, rgba, (size_t)t.width * t.height * 4);

	VKCHECK(vkResetCommandBuffer(m_cmd, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(m_cmd, &bi));

	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = t.image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b.oldLayout = t.everUploaded ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

	VkBufferImageCopy copy = {};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {t.width, t.height, 1};
	vkCmdCopyBufferToImage(m_cmd, t.staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	// SteamVR reads overlay textures in TRANSFER_SRC_OPTIMAL.
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	VKCHECK(vkEndCommandBuffer(m_cmd));

	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &m_cmd;
	VKCHECK(vkQueueSubmit(m_queue, 1, &si, m_fence));
	VKCHECK(vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 1000000000ull));
	VKCHECK(vkResetFences(m_device, 1, &m_fence));
	t.everUploaded = true;
	return true;
}

EVROverlayError VkShare::setOverlay(VROverlayHandle_t overlay, VkTexture &t) {
	VRVulkanTextureData_t data = {};
	data.m_nImage = (uint64_t)t.image;
	data.m_pDevice = m_device;
	data.m_pPhysicalDevice = m_phys;
	data.m_pInstance = m_instance;
	data.m_pQueue = m_queue;
	data.m_nQueueFamilyIndex = m_queueFamily;
	data.m_nWidth = t.width;
	data.m_nHeight = t.height;
	data.m_nFormat = VK_FORMAT_R8G8B8A8_SRGB;
	data.m_nSampleCount = 1;
	Texture_t tex = {&data, TextureType_Vulkan, ColorSpace_Auto};
	return VROverlay()->SetOverlayTexture(overlay, &tex);
}
