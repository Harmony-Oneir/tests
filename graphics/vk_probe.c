/*
 * vk_probe: проверка Vulkan-драйвера под OpenHarmony.
 *   1. создаёт VkInstance через загрузчик Vulkan OpenHarmony;
 *   2. перечисляет физические устройства (имя, драйвер, версия API);
 *   3. проверяет расширения, важные для Render Service;
 *   4. создаёт VkDevice с одной очередью графики.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char *KEY_EXTS[] = {
    "VK_OHOS_native_buffer",           /* обязательно для RS: изображения поверх буферов окон/экрана */
    "VK_OHOS_external_memory",         /* обязательно для RS: импорт OH_NativeBuffer */
    "VK_EXT_external_memory_dma_buf",  /* основа для импорта dma-buf */
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_semaphore_fd",    /* синхронизация через sync-fd */
    "VK_KHR_external_fence_fd",
    "VK_EXT_image_drm_format_modifier",
    "VK_KHR_swapchain",
};

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(e[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    uint32_t loader_ver = 0;
    vkEnumerateInstanceVersion(&loader_ver);
    printf("загрузчик Vulkan: %u.%u.%u\n", VK_VERSION_MAJOR(loader_ver),
           VK_VERSION_MINOR(loader_ver), VK_VERSION_PATCH(loader_ver));

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vk_probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("vkCreateInstance: ошибка %d\n", r);
        return 1;
    }

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    printf("физических устройств: %u\n", ndev);
    if (ndev == 0) {
        printf("драйвер не найден или не загрузился (проверьте VK_ICD_FILENAMES и VK_LOADER_DEBUG=all)\n");
        vkDestroyInstance(inst, NULL);
        return 2;
    }
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &ndev, devs);

    int rc = 0;
    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
        vkGetPhysicalDeviceProperties2(devs[d], &p2);
        VkPhysicalDeviceProperties *p = &p2.properties;
        printf("\n[%u] %s\n", d, p->deviceName);
        printf("    драйвер: %s (%s)\n", drv.driverName, drv.driverInfo);
        printf("    Vulkan API: %u.%u.%u\n", VK_VERSION_MAJOR(p->apiVersion),
               VK_VERSION_MINOR(p->apiVersion), VK_VERSION_PATCH(p->apiVersion));

        uint32_t next = 0;
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &next, NULL);
        VkExtensionProperties *ext = calloc(next, sizeof(*ext));
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &next, ext);
        printf("    расширений устройства: %u\n", next);
        for (size_t i = 0; i < sizeof(KEY_EXTS) / sizeof(KEY_EXTS[0]); i++) {
            printf("      %-36s %s\n", KEY_EXTS[i], has_ext(ext, next, KEY_EXTS[i]) ? "да" : "НЕТ");
        }
        free(ext);

        /* очередь с поддержкой графики */
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, NULL);
        VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        uint32_t qidx = UINT32_MAX;
        for (uint32_t i = 0; i < nq; i++) {
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                qidx = i;
                break;
            }
        }
        free(qf);
        if (qidx == UINT32_MAX) {
            printf("    нет очереди графики\n");
            rc = 3;
            continue;
        }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qidx,
            .queueCount = 1,
            .pQueuePriorities = &prio,
        };
        VkDeviceCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &qci,
        };
        VkDevice dev;
        r = vkCreateDevice(devs[d], &dci, NULL, &dev);
        printf("    vkCreateDevice: %s (%d)\n", r == VK_SUCCESS ? "успешно" : "ОШИБКА", r);
        if (r == VK_SUCCESS) {
            vkDestroyDevice(dev, NULL);
        } else {
            rc = 4;
        }
    }

    free(devs);
    vkDestroyInstance(inst, NULL);
    return rc;
}
