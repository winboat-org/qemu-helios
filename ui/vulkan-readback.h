#ifndef HELIOS_VULKAN_READBACK_H
#define HELIOS_VULKAN_READBACK_H

#include "ui/dmabuf.h"
#include "ui/surface.h"

typedef struct HeliosVulkanReadback HeliosVulkanReadback;
typedef struct HeliosVulkanReadbackCache HeliosVulkanReadbackCache;

HeliosVulkanReadback *helios_vulkan_readback_new(QemuDmaBuf *dmabuf,
                                                 bool direct_optimal);
bool helios_vulkan_readback_matches(HeliosVulkanReadback *readback,
                                    QemuDmaBuf *dmabuf,
                                    bool direct_optimal);
HeliosVulkanReadbackCache *helios_vulkan_readback_cache_new(void);
void helios_vulkan_readback_cache_free(HeliosVulkanReadbackCache *cache);
HeliosVulkanReadback *helios_vulkan_readback_cache_activate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf,
    bool direct_optimal);
void helios_vulkan_readback_cache_deactivate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf);
HeliosVulkanReadback *helios_vulkan_readback_cache_active(
    HeliosVulkanReadbackCache *cache);
void helios_vulkan_readback_free(HeliosVulkanReadback *readback);
bool helios_vulkan_readback_flush(HeliosVulkanReadback *readback,
                                  DisplaySurface *surface,
                                  uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height);

#endif
