/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "GpuSurfaceInternal.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSemaphore.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

namespace solar::gui {

namespace {

constexpr size_t kResourceBudget = static_cast<size_t>(64) << 20;

// The window drawn on through Vulkan: an instance, a device with a queue that can show to the window, a swapchain of images that Skia
// wraps as surfaces, one of which is the frame at a time.
class VulkanSurface final : public GpuSurface {
 public:
  ~VulkanSurface() override { Destroy(); }

  bool Init(SDL_Window* window) {
    window_ = window;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
    getInstanceProc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!getInstanceProc_) return false;
    if (!MakeInstance()) return false;
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) return false;
    if (!MakeDevice()) return false;
    skgpu::VulkanBackendContext backend;
    backend.fInstance = instance_;
    backend.fPhysicalDevice = physical_;
    backend.fDevice = device_;
    backend.fQueue = queue_;
    backend.fGraphicsQueueIndex = queueFamily_;
    backend.fMaxAPIVersion = apiVersion_;
    backend.fVkExtensions = &extensions_;
    backend.fDeviceFeatures = &features_;
    backend.fGetProc = [this](const char* name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
      if (device != VK_NULL_HANDLE && getDeviceProc_) return getDeviceProc_(device, name);
      return getInstanceProc_(instance, name);
    };
    backend.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
    if (!backend.fMemoryAllocator) return false;
    direct_ = GrDirectContexts::MakeVulkan(backend);
    if (!direct_) return false;
    direct_->setResourceCacheLimit(kResourceBudget);
    return true;
  }

  SkCanvas* BeginFrame(int pixelWidth, int pixelHeight) override {
    if (pixelWidth <= 0 || pixelHeight <= 0) return nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (swapchain_ == VK_NULL_HANDLE || stale_ || width_ != pixelWidth || height_ != pixelHeight) {
        if (!MakeSwapchain(pixelWidth, pixelHeight)) return nullptr;
      }
      VkSemaphoreCreateInfo semaphoreInfo{};
      semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      VkSemaphore acquired = VK_NULL_HANDLE;
      if (vkCreateSemaphore_(device_, &semaphoreInfo, nullptr, &acquired) != VK_SUCCESS) return nullptr;
      uint32_t index = 0;
      const VkResult result = vkAcquireNextImageKHR_(device_, swapchain_, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
      if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDestroySemaphore_(device_, acquired, nullptr);
        stale_ = true;
        continue;
      }
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        vkDestroySemaphore_(device_, acquired, nullptr);
        return nullptr;
      }
      if (result == VK_SUBOPTIMAL_KHR) stale_ = true;
      current_ = index;
      // (Skia waits for the image to be there, and deletes the semaphore after.)
      GrBackendSemaphore waitFor = GrBackendSemaphores::MakeVk(acquired);
      images_[index].surface->wait(1, &waitFor);
      return images_[index].surface->getCanvas();
    }
    return nullptr;
  }

  void Present() override {
    if (current_ == UINT32_MAX || current_ >= images_.size()) return;
    Image& image = images_[current_];
    GrBackendSemaphore signal = GrBackendSemaphores::MakeVk(image.rendered);
    GrFlushInfo info;
    info.fNumSemaphores = 1;
    info.fSignalSemaphores = &signal;
    direct_->flush(image.surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent, info);
    direct_->submit(GrSyncCpu::kNo);
    // (SOLAR_GUI_TIMES: the frame is waited for, to be able to tell what it cost the GPU.)
    static const bool wait = std::getenv("SOLAR_GUI_TIMES") != nullptr;
    if (wait) direct_->submit(GrSyncCpu::kYes);
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &current_;
    const VkResult result = vkQueuePresentKHR_(queue_, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) stale_ = true;
    current_ = UINT32_MAX;
  }

  bool ReadPixels(SkBitmap& out) override {
    if (current_ == UINT32_MAX || current_ >= images_.size()) return false;
    if (!out.tryAllocPixels(SkImageInfo::MakeN32Premul(width_, height_))) return false;
    return images_[current_].surface->readPixels(out, 0, 0);
  }

  const char* Renderer() const override { return renderer_.c_str(); }
  const char* Interface() const override { return "Vulkan"; }

  size_t ResourceBytes() const override {
    size_t bytes = 0;
    int count = 0;
    if (direct_) direct_->getResourceCacheUsage(&count, &bytes);
    return bytes;
  }

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;  // signaled when what was drawn on the image is done
    sk_sp<SkSurface> surface;
  };

  template <typename F>
  F InstanceProc(const char* name) const { return reinterpret_cast<F>(getInstanceProc_(instance_, name)); }
  template <typename F>
  F DeviceProc(const char* name) const { return reinterpret_cast<F>(getDeviceProc_(device_, name)); }

  bool MakeInstance() {
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(getInstanceProc_(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    uint32_t available = VK_API_VERSION_1_0;
    if (enumerateVersion) enumerateVersion(&available);
    // 1.1 is what Skia asks for at least; more is not needed.
    if (available < VK_API_VERSION_1_1) return false;
    apiVersion_ = VK_API_VERSION_1_1;
    Uint32 count = 0;
    const char* const* wanted = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!wanted) return false;
    for (Uint32 i = 0; i < count; ++i) instanceExtensions_.push_back(wanted[i]);
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Solar";
    application.apiVersion = apiVersion_;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions_.size());
    create.ppEnabledExtensionNames = instanceExtensions_.data();
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProc_(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance || createInstance(&create, nullptr, &instance_) != VK_SUCCESS) return false;
    return true;
  }

  bool MakeDevice() {
    auto enumerate = InstanceProc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    auto properties = InstanceProc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    auto families = InstanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    auto surfaceSupport = InstanceProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR");
    auto featuresOf = InstanceProc<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
    auto deviceExtensionsOf = InstanceProc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    auto createDevice = InstanceProc<PFN_vkCreateDevice>("vkCreateDevice");
    getDeviceProc_ = InstanceProc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    if (!enumerate || !properties || !families || !surfaceSupport || !featuresOf || !deviceExtensionsOf || !createDevice || !getDeviceProc_) return false;
    uint32_t count = 0;
    enumerate(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance_, &count, devices.data());
    // The first device with a queue that draws and shows to the window; a discrete one before the others.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosenFamily = 0;
    bool chosenDiscrete = false;
    for (VkPhysicalDevice candidate : devices) {
      VkPhysicalDeviceProperties props;
      properties(candidate, &props);
      if (props.apiVersion < VK_API_VERSION_1_1) continue;
      uint32_t familyCount = 0;
      families(candidate, &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> family(familyCount);
      families(candidate, &familyCount, family.data());
      for (uint32_t i = 0; i < familyCount; ++i) {
        VkBool32 shows = VK_FALSE;
        surfaceSupport(candidate, i, surface_, &shows);
        if (!(family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !shows) continue;
        const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (chosen == VK_NULL_HANDLE || (discrete && !chosenDiscrete)) {
          chosen = candidate;
          chosenFamily = i;
          chosenDiscrete = discrete;
          renderer_ = props.deviceName;
        }
        break;
      }
    }
    if (chosen == VK_NULL_HANDLE) return false;
    physical_ = chosen;
    queueFamily_ = chosenFamily;
    // The extensions: the swapchain, which is what shows, and none of what is optional.
    uint32_t extensionCount = 0;
    deviceExtensionsOf(physical_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    deviceExtensionsOf(physical_, nullptr, &extensionCount, available.data());
    bool hasSwapchain = false;
    for (const VkExtensionProperties& e : available) if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
    if (!hasSwapchain) return false;
    deviceExtensions_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    featuresOf(physical_, &features_);  // (what the device has is what is enabled: Skia uses what it finds)
    const float priority = 1.f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions_.size());
    create.ppEnabledExtensionNames = deviceExtensions_.data();
    create.pEnabledFeatures = &features_;
    if (createDevice(physical_, &create, nullptr, &device_) != VK_SUCCESS) return false;
    auto getQueue = DeviceProc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
    getQueue(device_, queueFamily_, 0, &queue_);
    vkCreateSemaphore_ = DeviceProc<PFN_vkCreateSemaphore>("vkCreateSemaphore");
    vkDestroySemaphore_ = DeviceProc<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    vkCreateSwapchainKHR_ = DeviceProc<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
    vkDestroySwapchainKHR_ = DeviceProc<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
    vkGetSwapchainImagesKHR_ = DeviceProc<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    vkAcquireNextImageKHR_ = DeviceProc<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
    vkQueuePresentKHR_ = DeviceProc<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    vkDeviceWaitIdle_ = DeviceProc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    vkGetSurfaceCapabilities_ = InstanceProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    vkGetSurfaceFormats_ = InstanceProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    vkGetSurfacePresentModes_ = InstanceProc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!getQueue || !vkCreateSemaphore_ || !vkDestroySemaphore_ || !vkCreateSwapchainKHR_ || !vkDestroySwapchainKHR_ || !vkGetSwapchainImagesKHR_ ||
        !vkAcquireNextImageKHR_ || !vkQueuePresentKHR_ || !vkDeviceWaitIdle_ || !vkGetSurfaceCapabilities_ || !vkGetSurfaceFormats_ || !vkGetSurfacePresentModes_) {
      return false;
    }
    extensions_.init(
        [this](const char* name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
          if (device != VK_NULL_HANDLE && getDeviceProc_) return getDeviceProc_(device, name);
          return getInstanceProc_(instance, name);
        },
        instance_, physical_, static_cast<uint32_t>(instanceExtensions_.size()), instanceExtensions_.data(), static_cast<uint32_t>(deviceExtensions_.size()),
        deviceExtensions_.data());
    return true;
  }

  void DestroyImages() {
    for (Image& image : images_) {
      image.surface.reset();
      if (image.rendered != VK_NULL_HANDLE) vkDestroySemaphore_(device_, image.rendered, nullptr);
    }
    images_.clear();
  }

  bool MakeSwapchain(int width, int height) {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle_(device_);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetSurfaceCapabilities_(physical_, surface_, &caps) != VK_SUCCESS) return false;
    uint32_t formatCount = 0;
    vkGetSurfaceFormats_(physical_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetSurfaceFormats_(physical_, surface_, &formatCount, formats.data());
    // A format that is not sRGB: what is drawn is in the numbers the page gave, and is not to be converted again on its way out.
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    SkColorType colorType = kBGRA_8888_SkColorType;
    for (const VkSurfaceFormatKHR& f : formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { format = f.format; colorSpace = f.colorSpace; colorType = kBGRA_8888_SkColorType; break; }
      if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f.format; colorSpace = f.colorSpace; colorType = kRGBA_8888_SkColorType; break; }
    }
    if (format == VK_FORMAT_UNDEFINED) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      extent.width = static_cast<uint32_t>(width);
      extent.height = static_cast<uint32_t>(height);
    }
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false;
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & usage) != usage) return false;
    VkSwapchainCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface = surface_;
    create.minImageCount = imageCount;
    create.imageFormat = format;
    create.imageColorSpace = colorSpace;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = usage;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // (waits for the display: always there)
    create.clipped = VK_TRUE;
    create.oldSwapchain = swapchain_;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR_(device_, &create, nullptr, &made) != VK_SUCCESS) return false;
    DestroyImages();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR_(device_, swapchain_, nullptr);
    swapchain_ = made;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR_(device_, swapchain_, &count, nullptr);
    std::vector<VkImage> vkImages(count);
    vkGetSwapchainImagesKHR_(device_, swapchain_, &count, vkImages.data());
    width_ = static_cast<int>(extent.width);
    height_ = static_cast<int>(extent.height);
    images_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      Image& image = images_[i];
      image.image = vkImages[i];
      VkSemaphoreCreateInfo semaphoreInfo{};
      semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      if (vkCreateSemaphore_(device_, &semaphoreInfo, nullptr, &image.rendered) != VK_SUCCESS) return false;
      GrVkImageInfo info;
      info.fImage = vkImages[i];
      info.fFormat = format;
      info.fImageUsageFlags = usage;
      info.fLevelCount = 1;
      info.fSampleCount = 1;
      info.fCurrentQueueFamily = queueFamily_;
      info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      const GrBackendRenderTarget target = GrBackendRenderTargets::MakeVk(width_, height_, info);
      image.surface = SkSurfaces::WrapBackendRenderTarget(direct_.get(), target, kTopLeft_GrSurfaceOrigin, colorType, nullptr, nullptr);
      if (!image.surface) return false;
    }
    stale_ = false;
    return true;
  }

  void Destroy() {
    if (device_ != VK_NULL_HANDLE && vkDeviceWaitIdle_) vkDeviceWaitIdle_(device_);
    DestroyImages();
    if (direct_) {
      direct_->releaseResourcesAndAbandonContext();
      direct_.reset();
    }
    if (swapchain_ != VK_NULL_HANDLE && vkDestroySwapchainKHR_) vkDestroySwapchainKHR_(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (device_ != VK_NULL_HANDLE) {
      if (auto destroyDevice = DeviceProc<PFN_vkDestroyDevice>("vkDestroyDevice")) destroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_ != VK_NULL_HANDLE) {
      if (auto destroyInstance = InstanceProc<PFN_vkDestroyInstance>("vkDestroyInstance")) destroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
  }

  SDL_Window* window_ = nullptr;
  PFN_vkGetInstanceProcAddr getInstanceProc_ = nullptr;
  PFN_vkGetDeviceProcAddr getDeviceProc_ = nullptr;
  uint32_t apiVersion_ = VK_API_VERSION_1_1;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  std::vector<const char*> instanceExtensions_, deviceExtensions_;
  VkPhysicalDeviceFeatures features_{};
  skgpu::VulkanExtensions extensions_;
  sk_sp<GrDirectContext> direct_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<Image> images_;
  uint32_t current_ = UINT32_MAX;
  int width_ = 0, height_ = 0;
  bool stale_ = false;
  std::string renderer_ = "Vulkan";

  PFN_vkCreateSemaphore vkCreateSemaphore_ = nullptr;
  PFN_vkDestroySemaphore vkDestroySemaphore_ = nullptr;
  PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR_ = nullptr;
  PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR_ = nullptr;
  PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR_ = nullptr;
  PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR_ = nullptr;
  PFN_vkQueuePresentKHR vkQueuePresentKHR_ = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle_ = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetSurfaceCapabilities_ = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetSurfaceFormats_ = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetSurfacePresentModes_ = nullptr;
};

}  // namespace

std::unique_ptr<GpuSurface> MakeVulkanSurface(SDL_Window* window) {
  if (!window) return nullptr;
  auto gpu = std::make_unique<VulkanSurface>();
  if (!gpu->Init(window)) return nullptr;
  return gpu;
}

}  // namespace solar::gui
