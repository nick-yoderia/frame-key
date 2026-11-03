// Minimal Vulkan setup for handing CPU-made images to SteamVR overlays.
// (SetOverlayRaw crashes this vrcompositor build when the overlay goes away.)
#pragma once
#include <openvr.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct VkTexture {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	void *mapped = nullptr;
	uint32_t width = 0, height = 0;
	bool everUploaded = false;
};

class VkShare {
public:
	bool init(); // call after VR_Init
	void shutdown();
	bool createTexture(uint32_t w, uint32_t h, VkTexture &t);
	void destroyTexture(VkTexture &t);
	// Copy RGBA8 pixels into the texture and wait until the GPU is done.
	bool upload(VkTexture &t, const uint8_t *rgba);
	// Point an overlay at the texture.
	vr::EVROverlayError setOverlay(vr::VROverlayHandle_t overlay, VkTexture &t);

	VkInstance instance() const { return m_instance; }
	VkPhysicalDevice physicalDevice() const { return m_phys; }
	VkDevice device() const { return m_device; }
	VkQueue queue() const { return m_queue; }
	uint32_t queueFamily() const { return m_queueFamily; }

private:
	uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags props);

	VkInstance m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_phys = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	uint32_t m_queueFamily = 0;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;
};
