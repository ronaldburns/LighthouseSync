// LighthouseSync panel: what SteamVR's compositor needs to show the panel's pixels. It takes an overlay's image as a
// Vulkan image, so this makes a device on the headset's GPU with the extensions SteamVR asks for, from the system's
// libvulkan.so.1 (loaded at run time, like openvr_shim.cpp's SteamVR), and copies the pixels into it.
#pragma once
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include <string>

namespace lhs::vk {

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;

  bool Init(std::string &err);  // after VR_Init: it asks SteamVR for the GPU and the extensions
  void Destroy();               // after VR_Shutdown, and after every Texture's Destroy
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
};

// an overlay's texture: two images it takes turns with, so SteamVR never reads the one being written
struct Texture {
  bool Create(Device &d, int w, int h, std::string &err);
  void Destroy();
  bool Ready() const { return d_ != nullptr; }
  // w*h RGBA pixels into the next image, then hands that image to the overlay. Waits for the copy (well under 1 ms)
  bool Set(uint64_t overlay, const void *rgba, std::string &err);

 private:
  Device *d_ = nullptr;
  int w_ = 0, h_ = 0, next_ = 0;
  VkImage img_[2] = {};
  VkDeviceMemory mem_[2] = {};
  bool used_[2] = {};
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
  void *mapped_ = nullptr;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace lhs::vk
