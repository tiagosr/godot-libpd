/* vk_probe.c — minimal Vulkan enumeration probe (instance + device + name/version). */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>
static PFN_vkGetInstanceProcAddr gipa;
int main(void) {
    void *lib = dlopen("libvulkan.so", 2);
    if (!lib) { printf("NO_LIBVULKAN\n"); return 2; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { printf("NO_GIPA\n"); return 3; }
    PFN_vkCreateInstance pci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "probe", .apiVersion = VK_MAKE_VERSION(1, 0, 0) };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai };
    VkInstance inst;
    VkResult r = pci(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { printf("CREATE_INSTANCE_FAIL=%d\n", r); return 4; }
    uint32_t nd = 0;
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    epd(inst, &nd, NULL);
    printf("DEVICES=%u\n", nd);
    if (nd == 0) return 5;
    VkPhysicalDevice devs[4];
    epd(inst, &nd, devs);
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    for (uint32_t i = 0; i < nd && i < 4; i++) {
        VkPhysicalDeviceProperties p;
        gpp(devs[i], &p);
        printf("DEV[%u]: %s apiVersion=%u.%u.%u driverVersion=%u vendor=0x%x device=0x%x\n",
               i, p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
               p.driverVersion, p.vendorID, p.deviceID);
    }
    return 0;
}
