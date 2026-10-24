#include "vk.h"

#include <dlfcn.h>
#include <string.h>

#include <sstream>
#include <vector>

#include "openvr.h"

namespace lhs::vk {

namespace {

// vkGetInstanceProcAddr finds the rest: the first group before there's an instance, the others with it
#define LHS_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define LHS_VK_INSTANCE(X)                                                                                        \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties)                \
  X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)              \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool)     \
  X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkCreateImage) X(vkDestroyImage)                        \
  X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                     \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)     \
  X(vkUnmapMemory) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences)                       \
  X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)                 \
  X(vkCmdCopyBufferToImage) X(vkQueueSubmit)

#define LHS_VK_DECLARE(f) PFN_##f f = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
LHS_VK_GLOBAL(LHS_VK_DECLARE)
LHS_VK_INSTANCE(LHS_VK_DECLARE)

const VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

std::string Fail(const char *what, VkResult r) { return std::string(what) + " failed (VkResult " + std::to_string((int)r) + ")"; }

// the names in SteamVR's space-separated list that this Vulkan has
std::vector<std::string> Supported(const char *wanted, const std::vector<VkExtensionProperties> &have) {
  std::vector<std::string> out;
  std::istringstream in(wanted);
  for (std::string name; in >> name;)
    for (const VkExtensionProperties &e : have)
      if (name == e.extensionName) {
        out.push_back(name);
        break;
      }
  return out;
}

std::vector<const char *> Ptrs(const std::vector<std::string> &v) {
  std::vector<const char *> out;
  for (const std::string &s : v) out.push_back(s.c_str());
  return out;
}

}  // namespace

bool Device::Init(std::string &err) {
  if (!vkGetInstanceProcAddr) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) {
      err = "libvulkan.so.1 wasn't found";
      return false;
    }
#define LHS_VK_LOAD_GLOBAL(f) f = (PFN_##f)vkGetInstanceProcAddr(nullptr, #f);
    LHS_VK_GLOBAL(LHS_VK_LOAD_GLOBAL)
  }
  char names[4096] = "";
  vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(names, sizeof names);
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
  std::vector<std::string> ext = Supported(names, have);
  std::vector<const char *> ptrs = Ptrs(ext);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "lighthousesync-panel";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ii.pApplicationInfo = &app;
  ii.enabledExtensionCount = (uint32_t)ptrs.size();
  ii.ppEnabledExtensionNames = ptrs.data();
  if (VkResult r = vkCreateInstance(&ii, nullptr, &instance)) {
    instance = VK_NULL_HANDLE;
    err = Fail("vkCreateInstance", r);
    return false;
  }
#define LHS_VK_LOAD(f) f = (PFN_##f)vkGetInstanceProcAddr(instance, #f);
  LHS_VK_INSTANCE(LHS_VK_LOAD)

  // the GPU SteamVR draws the headset's display with, else the first
  uint64_t out = 0;
  vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan, instance);
  gpu = (VkPhysicalDevice)out;
  if (!gpu) {
    uint32_t c = 1;
    vkEnumeratePhysicalDevices(instance, &c, &gpu);
  }
  if (!gpu) {
    err = "no Vulkan GPU";
    Destroy();
    return false;
  }
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
  std::vector<VkQueueFamilyProperties> fams(n);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, fams.data());
  family = UINT32_MAX;
  for (uint32_t i = 0; i < n && family == UINT32_MAX; i++)
    if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
  if (family == UINT32_MAX) {
    err = "no graphics queue";
    Destroy();
    return false;
  }

  memset(names, 0, sizeof names);
  vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(gpu, names, sizeof names);
  n = 0;
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
  have.resize(n);
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, have.data());
  ext = Supported(names, have);
  ptrs = Ptrs(ext);
  float prio = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = (uint32_t)ptrs.size();
  di.ppEnabledExtensionNames = ptrs.data();
  if (VkResult r = vkCreateDevice(gpu, &di, nullptr, &device)) {
    device = VK_NULL_HANDLE;
    err = Fail("vkCreateDevice", r);
    Destroy();
    return false;
  }
  vkGetDeviceQueue(device, family, 0, &queue);
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = family;
  if (VkResult r = vkCreateCommandPool(device, &pi, nullptr, &pool)) {
    pool = VK_NULL_HANDLE;
    err = Fail("vkCreateCommandPool", r);
    Destroy();
    return false;
  }
  return true;
}

void Device::Destroy() {
  if (device) {
    vkDeviceWaitIdle(device);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
  }
  if (instance) vkDestroyInstance(instance, nullptr);
  pool = VK_NULL_HANDLE;
  device = VK_NULL_HANDLE;
  queue = VK_NULL_HANDLE;
  gpu = VK_NULL_HANDLE;
  instance = VK_NULL_HANDLE;
}

uint32_t Device::MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

bool Texture::Create(Device &d, int w, int h, std::string &err) {
  Destroy();
  d_ = &d;
  w_ = w, h_ = h, next_ = 0;
  VkDevice dev = d.device;
  for (int i = 0; i < 2; i++) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFormat;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VkResult r = vkCreateImage(dev, &ci, nullptr, &img_[i])) {
      img_[i] = VK_NULL_HANDLE;
      err = Fail("vkCreateImage", r);
      Destroy();
      return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img_[i], &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = d.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (VkResult r = vkAllocateMemory(dev, &ai, nullptr, &mem_[i])) {
      mem_[i] = VK_NULL_HANDLE;
      err = Fail("vkAllocateMemory", r);
      Destroy();
      return false;
    }
    vkBindImageMemory(dev, img_[i], mem_[i], 0);
    used_[i] = false;
  }
  VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = bytes;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (VkResult r = vkCreateBuffer(dev, &bi, nullptr, &staging_)) {
    staging_ = VK_NULL_HANDLE;
    err = Fail("vkCreateBuffer", r);
    Destroy();
    return false;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, staging_, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      d.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (VkResult r = vkAllocateMemory(dev, &ai, nullptr, &staging_mem_)) {
    staging_mem_ = VK_NULL_HANDLE;
    err = Fail("vkAllocateMemory (staging)", r);
    Destroy();
    return false;
  }
  vkBindBufferMemory(dev, staging_, staging_mem_, 0);
  if (VkResult r = vkMapMemory(dev, staging_mem_, 0, bytes, 0, &mapped_)) {
    mapped_ = nullptr;
    err = Fail("vkMapMemory", r);
    Destroy();
    return false;
  }
  VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ci.commandPool = d.pool;
  ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ci.commandBufferCount = 1;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkAllocateCommandBuffers(dev, &ci, &cmd_) || vkCreateFence(dev, &fi, nullptr, &fence_)) {
    err = "couldn't make a command buffer";
    Destroy();
    return false;
  }
  return true;
}

void Texture::Destroy() {
  if (!d_ || !d_->device) {
    d_ = nullptr;
    return;
  }
  VkDevice dev = d_->device;
  vkDeviceWaitIdle(dev);
  if (fence_) vkDestroyFence(dev, fence_, nullptr);
  if (cmd_) vkFreeCommandBuffers(dev, d_->pool, 1, &cmd_);
  if (mapped_) vkUnmapMemory(dev, staging_mem_);
  if (staging_) vkDestroyBuffer(dev, staging_, nullptr);
  if (staging_mem_) vkFreeMemory(dev, staging_mem_, nullptr);
  for (int i = 0; i < 2; i++) {
    if (img_[i]) vkDestroyImage(dev, img_[i], nullptr);
    if (mem_[i]) vkFreeMemory(dev, mem_[i], nullptr);
    img_[i] = VK_NULL_HANDLE;
    mem_[i] = VK_NULL_HANDLE;
    used_[i] = false;
  }
  fence_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  mapped_ = nullptr;
  staging_ = VK_NULL_HANDLE;
  staging_mem_ = VK_NULL_HANDLE;
  d_ = nullptr;
}

bool Texture::Set(uint64_t overlay, const void *rgba, std::string &err) {
  if (!d_) {
    err = "no texture";
    return false;
  }
  int i = next_;
  next_ ^= 1;
  memcpy(mapped_, rgba, (size_t)w_ * h_ * 4);

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &begin);
  // SteamVR had it as TRANSFER_SRC (or it's new): into TRANSFER_DST for the copy, then back for SteamVR
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = used_[i] ? VK_ACCESS_TRANSFER_READ_BIT : 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = used_[i] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img_[i];
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {(uint32_t)w_, (uint32_t)h_, 1};
  vkCmdCopyBufferToImage(cmd_, staging_, img_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  vkEndCommandBuffer(cmd_);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkResetFences(d_->device, 1, &fence_);
  if (VkResult r = vkQueueSubmit(d_->queue, 1, &si, fence_)) {
    err = Fail("vkQueueSubmit", r);
    return false;
  }
  if (VkResult r = vkWaitForFences(d_->device, 1, &fence_, VK_TRUE, 1000000000ull)) {
    err = Fail("vkWaitForFences", r);
    return false;
  }
  used_[i] = true;

  vr::VRVulkanTextureData_t data{};
  data.m_nImage = (uint64_t)img_[i];
  data.m_pDevice = d_->device;
  data.m_pPhysicalDevice = d_->gpu;
  data.m_pInstance = d_->instance;
  data.m_pQueue = d_->queue;
  data.m_nQueueFamilyIndex = d_->family;
  data.m_nWidth = (uint32_t)w_;
  data.m_nHeight = (uint32_t)h_;
  data.m_nFormat = kFormat;
  data.m_nSampleCount = 1;
  vr::Texture_t tex{&data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};  // the pixels as they are
  if (vr::EVROverlayError e = vr::VROverlay()->SetOverlayTexture(overlay, &tex)) {
    err = std::string("SetOverlayTexture: ") + vr::VROverlay()->GetOverlayErrorNameFromEnum(e);
    return false;
  }
  return true;
}

}  // namespace lhs::vk
