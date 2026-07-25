#include "video.hpp"

static constexpr const char *kVideoSourcePath = "../../test.mp4";

Video::Video() {}

Video::~Video() {
    releaseDecoder();
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
}

SDL_Texture *Video::GetTexture() const {
    return m_texture;
}

int Video::GetTextureWidth() const {
    return m_textureWidth;
}

int Video::GetTextureHeight() const {
    return m_textureHeight;
}

void Video::releaseDecoder() {
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
    m_videoStreamIndex = -1;
}

bool Video::ensureDecoder(SDL_Renderer *renderer) {
    if (m_codecContext) {
        return true;
    }

    (void)renderer;

    if (!vulkan_context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan video context is not ready");
        return false;
    }

    SDL_Log("Opening video source: %s", kVideoSourcePath);
    if (avformat_open_input(&m_formatContext, kVideoSourcePath, NULL, NULL) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't open %s", kVideoSourcePath);
        releaseDecoder();
        return false;
    }

    if (avformat_find_stream_info(m_formatContext, NULL) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't read stream info from %s", kVideoSourcePath);
        releaseDecoder();
        return false;
    }

    const AVCodec *videoCodec = NULL;
    m_videoStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0);
    if (m_videoStreamIndex < 0 || !videoCodec) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't find a video stream in %s", kVideoSourcePath);
        releaseDecoder();
        return false;
    }

    m_codecContext = OpenVideoStream(m_formatContext, m_videoStreamIndex, videoCodec);
    if (!m_codecContext) {
        releaseDecoder();
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_packet || !m_frame) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate FFmpeg packet or frame");
        releaseDecoder();
        return false;
    }

    return true;
}

static SDL_PixelFormat GetTextureFormat(enum AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_RGB8:
        return SDL_PIXELFORMAT_RGB332;
    case AV_PIX_FMT_RGB444:
        return SDL_PIXELFORMAT_XRGB4444;
    case AV_PIX_FMT_RGB555:
        return SDL_PIXELFORMAT_XRGB1555;
    case AV_PIX_FMT_BGR555:
        return SDL_PIXELFORMAT_XBGR1555;
    case AV_PIX_FMT_RGB565:
        return SDL_PIXELFORMAT_RGB565;
    case AV_PIX_FMT_BGR565:
        return SDL_PIXELFORMAT_BGR565;
    case AV_PIX_FMT_RGB24:
        return SDL_PIXELFORMAT_RGB24;
    case AV_PIX_FMT_BGR24:
        return SDL_PIXELFORMAT_BGR24;
    case AV_PIX_FMT_0RGB32:
        return SDL_PIXELFORMAT_XRGB8888;
    case AV_PIX_FMT_0BGR32:
        return SDL_PIXELFORMAT_XBGR8888;
    case AV_PIX_FMT_NE(RGB0, 0BGR):
        return SDL_PIXELFORMAT_RGBX8888;
    case AV_PIX_FMT_NE(BGR0, 0RGB):
        return SDL_PIXELFORMAT_BGRX8888;
    case AV_PIX_FMT_RGB32:
        return SDL_PIXELFORMAT_ARGB8888;
    case AV_PIX_FMT_RGB32_1:
        return SDL_PIXELFORMAT_RGBA8888;
    case AV_PIX_FMT_BGR32:
        return SDL_PIXELFORMAT_ABGR8888;
    case AV_PIX_FMT_BGR32_1:
        return SDL_PIXELFORMAT_BGRA8888;
    case AV_PIX_FMT_YUV420P:
        return SDL_PIXELFORMAT_IYUV;
    case AV_PIX_FMT_YUYV422:
        return SDL_PIXELFORMAT_YUY2;
    case AV_PIX_FMT_UYVY422:
        return SDL_PIXELFORMAT_UYVY;
    case AV_PIX_FMT_NV12:
        return SDL_PIXELFORMAT_NV12;
    case AV_PIX_FMT_NV21:
        return SDL_PIXELFORMAT_NV21;
    case AV_PIX_FMT_P010:
        return SDL_PIXELFORMAT_P010;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

void Video::SetupVulkanRenderProperties(VulkanVideoContext *context, SDL_PropertiesID props) {
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER, context->instance);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_SURFACE_NUMBER, (Sint64)context->surface);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_PHYSICAL_DEVICE_POINTER, context->physicalDevice);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_DEVICE_POINTER, context->device);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_PRESENT_QUEUE_FAMILY_INDEX_NUMBER, context->presentQueueFamilyIndex);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER, context->graphicsQueueFamilyIndex);
}

static void initDeviceFeatures(VulkanDeviceFeatures *features) {
    features->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features->device_features.pNext = &features->device_features_1_1;
    features->device_features_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features->device_features_1_1.pNext = &features->device_features_1_2;
    features->device_features_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features->device_features_1_2.pNext = &features->device_features_1_3;
    features->device_features_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features->device_features_1_3.pNext = &features->desc_buf_features;
    features->desc_buf_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    features->desc_buf_features.pNext = &features->atomic_float_features;
    features->atomic_float_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    features->atomic_float_features.pNext = &features->coop_matrix_features;
    features->coop_matrix_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    features->coop_matrix_features.pNext = NULL;
}

static int addQueueFamily(VkDeviceQueueCreateInfo **pQueueCreateInfos, uint32_t *pQueueCreateInfoCount, uint32_t queueFamilyIndex, uint32_t queueCount) {
    VkDeviceQueueCreateInfo *queueCreateInfo;
    VkDeviceQueueCreateInfo *queueCreateInfos = *pQueueCreateInfos;
    uint32_t queueCreateInfoCount = *pQueueCreateInfoCount;
    float *queuePriorities;

    if (queueCount == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < queueCreateInfoCount; ++i) {
        if (queueCreateInfos[i].queueFamilyIndex == queueFamilyIndex) {
            return 0;
        }
    }

    queueCreateInfos = (VkDeviceQueueCreateInfo *)SDL_realloc(queueCreateInfos, (queueCreateInfoCount + 1) * sizeof(*queueCreateInfos));
    if (!queueCreateInfos) {
        return -1;
    }

    queuePriorities = (float *)SDL_malloc(queueCount * sizeof(*queuePriorities));
    if (!queuePriorities) {
        return -1;
    }

    for (uint32_t i = 0; i < queueCount; ++i) {
        queuePriorities[i] = 1.0f / queueCount;
    }

    queueCreateInfo = &queueCreateInfos[queueCreateInfoCount++];
    SDL_zerop(queueCreateInfo);
    queueCreateInfo->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo->queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo->queueCount = queueCount;
    queueCreateInfo->pQueuePriorities = queuePriorities;

    *pQueueCreateInfos = queueCreateInfos;
    *pQueueCreateInfoCount = queueCreateInfoCount;
    return 0;
}

static int loadDeviceFunctions(VulkanVideoContext *context) {
#define VULKAN_GLOBAL_FUNCTION(name)
#define VULKAN_INSTANCE_FUNCTION(name)
#define VULKAN_DEVICE_FUNCTION(name)                                                                                                                 \
    context->name = (PFN_##name)context->vkGetDeviceProcAddr(context->device, #name);                                                                \
    if (!context->name) {                                                                                                                            \
        return SDL_SetError("vkGetDeviceProcAddr(device, \"" #name "\") failed");                                                                    \
    }
    VULKAN_FUNCTIONS()
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION
#undef VULKAN_DEVICE_FUNCTION
    return 0;
}

// Use the same queue scoring algorithm as ffmpeg to make sure we get the same device configuration
static int selectQueueFamily(VkQueueFamilyProperties *queueFamiliesProperties, uint32_t queueFamiliesCount, VkQueueFlags flags, int *queueCount) {
    uint32_t queueFamilyIndex;
    uint32_t selectedQueueFamilyIndex = queueFamiliesCount;
    uint32_t min_score = ~0u;

    for (queueFamilyIndex = 0; queueFamilyIndex < queueFamiliesCount; ++queueFamilyIndex) {
        VkQueueFlags current_flags = queueFamiliesProperties[queueFamilyIndex].queueFlags;
        if (current_flags & flags) {
            uint32_t score = av_popcount(current_flags) + queueFamiliesProperties[queueFamilyIndex].timestampValidBits;
            if (score < min_score) {
                selectedQueueFamilyIndex = queueFamilyIndex;
                min_score = score;
            }
        }
    }

    if (selectedQueueFamilyIndex != queueFamiliesCount) {
        VkQueueFamilyProperties *selectedQueueFamily = &queueFamiliesProperties[selectedQueueFamilyIndex];
        *queueCount = (int)selectedQueueFamily->queueCount;
        ++selectedQueueFamily->timestampValidBits;
        return (int)selectedQueueFamilyIndex;
    } else {
        *queueCount = 0;
        return -1;
    }
}

static int loadGlobalFunctions(VulkanVideoContext *context) {
    context->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!context->vkGetInstanceProcAddr) {
        return -1;
    }

#define VULKAN_GLOBAL_FUNCTION(name)                                                                                                                 \
    context->name = (PFN_##name)context->vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);                                                               \
    if (!context->name) {                                                                                                                            \
        return SDL_SetError("vkGetInstanceProcAddr(VK_NULL_HANDLE, \"" #name "\") failed");                                                          \
    }
#define VULKAN_INSTANCE_FUNCTION(name)
#define VULKAN_DEVICE_FUNCTION(name)
    VULKAN_FUNCTIONS()
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION
#undef VULKAN_DEVICE_FUNCTION
    return 0;
}

static int loadInstanceFunctions(VulkanVideoContext *context) {
#define VULKAN_GLOBAL_FUNCTION(name)
#define VULKAN_INSTANCE_FUNCTION(name)                                                                                                               \
    context->name = (PFN_##name)context->vkGetInstanceProcAddr(context->instance, #name);                                                            \
    if (!context->name) {                                                                                                                            \
        return SDL_SetError("vkGetInstanceProcAddr(instance, \"" #name "\") failed");                                                                \
    }
#define VULKAN_DEVICE_FUNCTION(name)
    VULKAN_FUNCTIONS()
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION
#undef VULKAN_DEVICE_FUNCTION
    return 0;
}

static const char *getVulkanResultString(VkResult result) {
    switch ((int)result) {
#define RESULT_CASE(x)                                                                                                                               \
    case x:                                                                                                                                          \
        return #x
        RESULT_CASE(VK_SUCCESS);
        RESULT_CASE(VK_NOT_READY);
        RESULT_CASE(VK_TIMEOUT);
        RESULT_CASE(VK_EVENT_SET);
        RESULT_CASE(VK_EVENT_RESET);
        RESULT_CASE(VK_INCOMPLETE);
        RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY);
        RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED);
        RESULT_CASE(VK_ERROR_DEVICE_LOST);
        RESULT_CASE(VK_ERROR_MEMORY_MAP_FAILED);
        RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT);
        RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT);
        RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT);
        RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER);
        RESULT_CASE(VK_ERROR_TOO_MANY_OBJECTS);
        RESULT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED);
        RESULT_CASE(VK_ERROR_FRAGMENTED_POOL);
        RESULT_CASE(VK_ERROR_SURFACE_LOST_KHR);
        RESULT_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
        RESULT_CASE(VK_SUBOPTIMAL_KHR);
        RESULT_CASE(VK_ERROR_OUT_OF_DATE_KHR);
        RESULT_CASE(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR);
        RESULT_CASE(VK_ERROR_VALIDATION_FAILED_EXT);
        RESULT_CASE(VK_ERROR_OUT_OF_POOL_MEMORY_KHR);
        RESULT_CASE(VK_ERROR_INVALID_SHADER_NV);
#undef RESULT_CASE
    default:
        break;
    }
    return (result < 0) ? "VK_ERROR_<Unknown>" : "VK_<Unknown>";
}

static int createInstance(VulkanVideoContext *context) {
    static const char *optional_extensions[] = {VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkApplicationInfo appInfo;
    SDL_zero(appInfo);
    VkInstanceCreateInfo instanceCreateInfo;
    SDL_zero(instanceCreateInfo);
    VkResult result;
    char const *const *instanceExtensions = SDL_Vulkan_GetInstanceExtensions(&instanceCreateInfo.enabledExtensionCount);

    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &appInfo;

    const char **instanceExtensionsCopy =
        (const char **)SDL_calloc(instanceCreateInfo.enabledExtensionCount + SDL_arraysize(optional_extensions), sizeof(const char *));
    for (uint32_t i = 0; i < instanceCreateInfo.enabledExtensionCount; i++) {
        instanceExtensionsCopy[i] = instanceExtensions[i];
    }

    // Get the rest of the optional extensions
    {
        uint32_t extensionCount;
        if (context->vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, NULL) == VK_SUCCESS && extensionCount > 0) {
            VkExtensionProperties *extensionProperties = (VkExtensionProperties *)SDL_calloc(extensionCount, sizeof(VkExtensionProperties));
            if (context->vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, extensionProperties) == VK_SUCCESS) {
                for (uint32_t i = 0; i < SDL_arraysize(optional_extensions); ++i) {
                    for (uint32_t j = 0; j < extensionCount; ++j) {
                        if (SDL_strcmp(extensionProperties[j].extensionName, optional_extensions[i]) == 0) {
                            instanceExtensionsCopy[instanceCreateInfo.enabledExtensionCount++] = optional_extensions[i];
                            break;
                        }
                    }
                }
            }
            SDL_free(extensionProperties);
        }
    }
    instanceCreateInfo.ppEnabledExtensionNames = instanceExtensionsCopy;

    context->instanceExtensions = instanceExtensionsCopy;
    context->instanceExtensionsCount = instanceCreateInfo.enabledExtensionCount;

    result = context->vkCreateInstance(&instanceCreateInfo, NULL, &context->instance);
    if (result != VK_SUCCESS) {
        context->instance = VK_NULL_HANDLE;
        return SDL_SetError("vkCreateInstance(): %s", getVulkanResultString(result));
    }
    if (loadInstanceFunctions(context) < 0) {
        return -1;
    }
    return 0;
}

static int createSurface(VulkanVideoContext *context, SDL_Window *window) {
    if (!SDL_Vulkan_CreateSurface(window, context->instance, NULL, &context->surface)) {
        context->surface = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

static void copyDeviceFeatures(VulkanDeviceFeatures *supported_features, VulkanDeviceFeatures *requested_features) {
#define COPY_OPTIONAL_FEATURE(X) requested_features->X = supported_features->X
    COPY_OPTIONAL_FEATURE(device_features.features.shaderImageGatherExtended);
    COPY_OPTIONAL_FEATURE(device_features.features.shaderStorageImageReadWithoutFormat);
    COPY_OPTIONAL_FEATURE(device_features.features.shaderStorageImageWriteWithoutFormat);
    COPY_OPTIONAL_FEATURE(device_features.features.fragmentStoresAndAtomics);
    COPY_OPTIONAL_FEATURE(device_features.features.vertexPipelineStoresAndAtomics);
    COPY_OPTIONAL_FEATURE(device_features.features.shaderInt64);
    COPY_OPTIONAL_FEATURE(device_features.features.shaderInt16);
    COPY_OPTIONAL_FEATURE(device_features.features.shaderFloat64);
    COPY_OPTIONAL_FEATURE(device_features_1_1.samplerYcbcrConversion);
    COPY_OPTIONAL_FEATURE(device_features_1_1.storagePushConstant16);
    COPY_OPTIONAL_FEATURE(device_features_1_2.bufferDeviceAddress);
    COPY_OPTIONAL_FEATURE(device_features_1_2.hostQueryReset);
    COPY_OPTIONAL_FEATURE(device_features_1_2.storagePushConstant8);
    COPY_OPTIONAL_FEATURE(device_features_1_2.shaderInt8);
    COPY_OPTIONAL_FEATURE(device_features_1_2.storageBuffer8BitAccess);
    COPY_OPTIONAL_FEATURE(device_features_1_2.uniformAndStorageBuffer8BitAccess);
    COPY_OPTIONAL_FEATURE(device_features_1_2.shaderFloat16);
    COPY_OPTIONAL_FEATURE(device_features_1_2.shaderSharedInt64Atomics);
    COPY_OPTIONAL_FEATURE(device_features_1_2.vulkanMemoryModel);
    COPY_OPTIONAL_FEATURE(device_features_1_2.vulkanMemoryModelDeviceScope);
    COPY_OPTIONAL_FEATURE(device_features_1_2.hostQueryReset);
    COPY_OPTIONAL_FEATURE(device_features_1_3.dynamicRendering);
    COPY_OPTIONAL_FEATURE(device_features_1_3.maintenance4);
    COPY_OPTIONAL_FEATURE(device_features_1_3.synchronization2);
    COPY_OPTIONAL_FEATURE(device_features_1_3.computeFullSubgroups);
    COPY_OPTIONAL_FEATURE(device_features_1_3.shaderZeroInitializeWorkgroupMemory);
    COPY_OPTIONAL_FEATURE(desc_buf_features.descriptorBuffer);
    COPY_OPTIONAL_FEATURE(desc_buf_features.descriptorBufferPushDescriptors);
    COPY_OPTIONAL_FEATURE(atomic_float_features.shaderBufferFloat32Atomics);
    COPY_OPTIONAL_FEATURE(atomic_float_features.shaderBufferFloat32AtomicAdd);
    COPY_OPTIONAL_FEATURE(coop_matrix_features.cooperativeMatrix);
#undef COPY_OPTIONAL_FEATURE

    // timeline semaphores is required by ffmpeg
    requested_features->device_features_1_2.timelineSemaphore = 1;
}

static int findPhysicalDevice(VulkanVideoContext *context) {
    uint32_t physicalDeviceCount = 0;
    VkPhysicalDevice *physicalDevices;
    VkQueueFamilyProperties *queueFamiliesProperties = NULL;
    uint32_t queueFamiliesPropertiesAllocatedSize = 0;
    VkExtensionProperties *deviceExtensions = NULL;
    uint32_t deviceExtensionsAllocatedSize = 0;
    uint32_t physicalDeviceIndex;
    VkResult result;

    result = context->vkEnumeratePhysicalDevices(context->instance, &physicalDeviceCount, NULL);
    if (result != VK_SUCCESS) {
        return SDL_SetError("vkEnumeratePhysicalDevices(): %s", getVulkanResultString(result));
    }
    if (physicalDeviceCount == 0) {
        return SDL_SetError("vkEnumeratePhysicalDevices(): no physical devices");
    }
    physicalDevices = (VkPhysicalDevice *)SDL_malloc(sizeof(VkPhysicalDevice) * physicalDeviceCount);
    if (!physicalDevices) {
        return -1;
    }
    result = context->vkEnumeratePhysicalDevices(context->instance, &physicalDeviceCount, physicalDevices);
    if (result != VK_SUCCESS) {
        SDL_free(physicalDevices);
        return SDL_SetError("vkEnumeratePhysicalDevices(): %s", getVulkanResultString(result));
    }
    context->physicalDevice = NULL;
    for (physicalDeviceIndex = 0; physicalDeviceIndex < physicalDeviceCount; physicalDeviceIndex++) {
        uint32_t queueFamiliesCount = 0;
        uint32_t queueFamilyIndex;
        uint32_t deviceExtensionCount = 0;
        bool hasSwapchainExtension = false;
        uint32_t i;

        VkPhysicalDevice physicalDevice = physicalDevices[physicalDeviceIndex];
        context->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamiliesCount, NULL);
        if (queueFamiliesCount == 0) {
            continue;
        }
        if (queueFamiliesPropertiesAllocatedSize < queueFamiliesCount) {
            SDL_free(queueFamiliesProperties);
            queueFamiliesPropertiesAllocatedSize = queueFamiliesCount;
            queueFamiliesProperties = (VkQueueFamilyProperties *)SDL_malloc(sizeof(VkQueueFamilyProperties) * queueFamiliesPropertiesAllocatedSize);
            if (!queueFamiliesProperties) {
                SDL_free(physicalDevices);
                SDL_free(deviceExtensions);
                return -1;
            }
        }
        context->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamiliesCount, queueFamiliesProperties);

        // Initialize timestampValidBits for scoring in selectQueueFamily
        for (queueFamilyIndex = 0; queueFamilyIndex < queueFamiliesCount; queueFamilyIndex++) {
            queueFamiliesProperties[queueFamilyIndex].timestampValidBits = 0;
        }
        context->presentQueueFamilyIndex = -1;
        context->graphicsQueueFamilyIndex = -1;
        for (queueFamilyIndex = 0; queueFamilyIndex < queueFamiliesCount; queueFamilyIndex++) {
            VkBool32 supported = 0;

            if (queueFamiliesProperties[queueFamilyIndex].queueCount == 0) {
                continue;
            }

            if (queueFamiliesProperties[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                context->graphicsQueueFamilyIndex = queueFamilyIndex;
            }

            result = context->vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, context->surface, &supported);
            if (result == VK_SUCCESS) {
                if (supported) {
                    context->presentQueueFamilyIndex = queueFamilyIndex;
                    if (queueFamiliesProperties[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                        break; // use this queue because it can present and do graphics
                    }
                }
            }
        }
        if (context->presentQueueFamilyIndex < 0 || context->graphicsQueueFamilyIndex < 0) {
            // We can't render and present on this device
            continue;
        }

        context->presentQueueCount = queueFamiliesProperties[context->presentQueueFamilyIndex].queueCount;
        ++queueFamiliesProperties[context->presentQueueFamilyIndex].timestampValidBits;
        context->graphicsQueueCount = queueFamiliesProperties[context->graphicsQueueFamilyIndex].queueCount;
        ++queueFamiliesProperties[context->graphicsQueueFamilyIndex].timestampValidBits;

        context->transferQueueFamilyIndex =
            selectQueueFamily(queueFamiliesProperties, queueFamiliesCount, VK_QUEUE_TRANSFER_BIT, &context->transferQueueCount);
        context->computeQueueFamilyIndex =
            selectQueueFamily(queueFamiliesProperties, queueFamiliesCount, VK_QUEUE_COMPUTE_BIT, &context->computeQueueCount);
        context->decodeQueueFamilyIndex =
            selectQueueFamily(queueFamiliesProperties, queueFamiliesCount, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &context->decodeQueueCount);
        if (context->transferQueueFamilyIndex < 0) {
            // ffmpeg can fall back to the compute or graphics queues for this
            context->transferQueueFamilyIndex =
                selectQueueFamily(queueFamiliesProperties, queueFamiliesCount, VK_QUEUE_COMPUTE_BIT, &context->transferQueueCount);
            if (context->transferQueueFamilyIndex < 0) {
                context->transferQueueFamilyIndex =
                    selectQueueFamily(queueFamiliesProperties, queueFamiliesCount, VK_QUEUE_GRAPHICS_BIT, &context->transferQueueCount);
            }
        }

        if (context->transferQueueFamilyIndex < 0 || context->computeQueueFamilyIndex < 0) {
            // This device doesn't have the queues we need for video decoding
            continue;
        }

        result = context->vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, NULL);
        if (result != VK_SUCCESS) {
            SDL_free(physicalDevices);
            SDL_free(queueFamiliesProperties);
            SDL_free(deviceExtensions);
            return SDL_SetError("vkEnumerateDeviceExtensionProperties(): %s", getVulkanResultString(result));
        }
        if (deviceExtensionCount == 0) {
            continue;
        }
        if (deviceExtensionsAllocatedSize < deviceExtensionCount) {
            SDL_free(deviceExtensions);
            deviceExtensionsAllocatedSize = deviceExtensionCount;
            deviceExtensions = (VkExtensionProperties *)SDL_malloc(sizeof(VkExtensionProperties) * deviceExtensionsAllocatedSize);
            if (!deviceExtensions) {
                SDL_free(physicalDevices);
                SDL_free(queueFamiliesProperties);
                return -1;
            }
        }
        result = context->vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, deviceExtensions);
        if (result != VK_SUCCESS) {
            SDL_free(physicalDevices);
            SDL_free(queueFamiliesProperties);
            SDL_free(deviceExtensions);
            return SDL_SetError("vkEnumerateDeviceExtensionProperties(): %s", getVulkanResultString(result));
        }
        for (i = 0; i < deviceExtensionCount; i++) {
            if (SDL_strcmp(deviceExtensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchainExtension = true;
                break;
            }
        }
        if (!hasSwapchainExtension) {
            continue;
        }
        context->physicalDevice = physicalDevice;
        break;
    }
    SDL_free(physicalDevices);
    SDL_free(queueFamiliesProperties);
    SDL_free(deviceExtensions);
    if (!context->physicalDevice) {
        return SDL_SetError("Vulkan: no viable physical devices found");
    }
    return 0;
}

static int createDevice(VulkanVideoContext *context) {
    static const char *const deviceExtensionNames[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    static const char *optional_extensions[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME
    };
    VkDeviceCreateInfo deviceCreateInfo;
    VkDeviceQueueCreateInfo *queueCreateInfos = NULL;
    uint32_t queueCreateInfoCount = 0;
    VulkanDeviceFeatures supported_features;
    const char **deviceExtensionsCopy = NULL;
    VkResult result = VK_ERROR_UNKNOWN;

    if (addQueueFamily(&queueCreateInfos, &queueCreateInfoCount, context->presentQueueFamilyIndex, context->presentQueueCount) < 0 ||
        addQueueFamily(&queueCreateInfos, &queueCreateInfoCount, context->graphicsQueueFamilyIndex, context->graphicsQueueCount) < 0 ||
        addQueueFamily(&queueCreateInfos, &queueCreateInfoCount, context->transferQueueFamilyIndex, context->transferQueueCount) < 0 ||
        addQueueFamily(&queueCreateInfos, &queueCreateInfoCount, context->computeQueueFamilyIndex, context->computeQueueCount) < 0 ||
        addQueueFamily(&queueCreateInfos, &queueCreateInfoCount, context->decodeQueueFamilyIndex, context->decodeQueueCount) < 0) {
        goto done;
    }

    initDeviceFeatures(&supported_features);
    initDeviceFeatures(&context->features);
    context->vkGetPhysicalDeviceFeatures2(context->physicalDevice, &supported_features.device_features);
    copyDeviceFeatures(&supported_features, &context->features);

    SDL_zero(deviceCreateInfo);
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = queueCreateInfoCount;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos;
    deviceCreateInfo.pEnabledFeatures = NULL;
    deviceCreateInfo.enabledExtensionCount = SDL_arraysize(deviceExtensionNames);
    deviceCreateInfo.pNext = &context->features.device_features;

    deviceExtensionsCopy =
        (const char **)SDL_calloc(deviceCreateInfo.enabledExtensionCount + SDL_arraysize(optional_extensions), sizeof(const char *));
    for (uint32_t i = 0; i < deviceCreateInfo.enabledExtensionCount; i++) {
        deviceExtensionsCopy[i] = deviceExtensionNames[i];
    }

    // Get the rest of the optional extensions
    {
        uint32_t extensionCount;
        if (context->vkEnumerateDeviceExtensionProperties(context->physicalDevice, NULL, &extensionCount, NULL) == VK_SUCCESS && extensionCount > 0) {
            VkExtensionProperties *extensionProperties = (VkExtensionProperties *)SDL_calloc(extensionCount, sizeof(VkExtensionProperties));
            if (context->vkEnumerateDeviceExtensionProperties(context->physicalDevice, NULL, &extensionCount, extensionProperties) == VK_SUCCESS) {
                for (uint32_t i = 0; i < SDL_arraysize(optional_extensions); ++i) {
                    for (uint32_t j = 0; j < extensionCount; ++j) {
                        if (SDL_strcmp(extensionProperties[j].extensionName, optional_extensions[i]) == 0) {
                            deviceExtensionsCopy[deviceCreateInfo.enabledExtensionCount++] = optional_extensions[i];
                            break;
                        }
                    }
                }
            }
            SDL_free(extensionProperties);
        }
    }
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensionsCopy;

    context->deviceExtensions = deviceExtensionsCopy;
    context->deviceExtensionsCount = deviceCreateInfo.enabledExtensionCount;

    result = context->vkCreateDevice(context->physicalDevice, &deviceCreateInfo, NULL, &context->device);
    if (result != VK_SUCCESS) {
        SDL_SetError("vkCreateDevice(): %s", getVulkanResultString(result));
        goto done;
    }

    if (loadDeviceFunctions(context) < 0) {
        result = VK_ERROR_UNKNOWN;
        context->device = VK_NULL_HANDLE;
        goto done;
    }

    // Get the graphics queue that SDL will use
    context->vkGetDeviceQueue(context->device, context->graphicsQueueFamilyIndex, 0, &context->graphicsQueue);

    // Create a command pool
    VkCommandPoolCreateInfo commandPoolCreateInfo;
    SDL_zero(commandPoolCreateInfo);
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = context->graphicsQueueFamilyIndex;
    result = context->vkCreateCommandPool(context->device, &commandPoolCreateInfo, NULL, &context->commandPool);
    if (result != VK_SUCCESS) {
        SDL_SetError("vkCreateCommandPool(): %s", getVulkanResultString(result));
        goto done;
    }

done:
    for (uint32_t i = 0; i < queueCreateInfoCount; ++i) {
        SDL_free((void *)queueCreateInfos[i].pQueuePriorities);
    }
    SDL_free(queueCreateInfos);

    if (result != VK_SUCCESS) {
        return -1;
    }
    return 0;
}

void DestroyVulkanVideoContext(VulkanVideoContext *context) {
    if (context) {
        if (context->device) {
            context->vkDeviceWaitIdle(context->device);
        }
        SDL_free(context->instanceExtensions);
        SDL_free(context->deviceExtensions);
        if (context->commandBuffers) {
            context->vkFreeCommandBuffers(context->device, context->commandPool, context->commandBufferCount, context->commandBuffers);
            SDL_free(context->commandBuffers);
            context->commandBuffers = NULL;
        }
        if (context->commandPool) {
            context->vkDestroyCommandPool(context->device, context->commandPool, NULL);
            context->commandPool = VK_NULL_HANDLE;
        }
        if (context->device) {
            context->vkDestroyDevice(context->device, NULL);
        }
        if (context->surface) {
            context->vkDestroySurfaceKHR(context->instance, context->surface, NULL);
        }
        if (context->instance) {
            context->vkDestroyInstance(context->instance, NULL);
        }
        SDL_free(context);
    }
}

VulkanVideoContext *Video::CreateVulkanVideoContext(SDL_Window *window) {
    VulkanVideoContext *context = (VulkanVideoContext *)SDL_calloc(1, sizeof(*context));
    if (!context) {
        return NULL;
    }
    if (loadGlobalFunctions(context) < 0 || createInstance(context) < 0 || createSurface(context, window) < 0 || findPhysicalDevice(context) < 0 ||
        createDevice(context) < 0) {
        DestroyVulkanVideoContext(context);
        return NULL;
    }
    return context;
}

static SDL_Colorspace GetFrameColorspace(AVFrame *frame)
{
    SDL_Colorspace colorspace = SDL_COLORSPACE_SRGB;

    if (frame && frame->colorspace != AVCOL_SPC_RGB) {
        if (frame->colorspace != AVCOL_SPC_UNSPECIFIED) {
#ifdef DEBUG_COLORSPACE
            SDL_Log("Frame colorspace: range: %d, primaries: %d, trc: %d, colorspace: %d, chroma_location: %d", frame->color_range, frame->color_primaries, frame->color_trc, frame->colorspace, frame->chroma_location);
#endif
            colorspace = (SDL_Colorspace)SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR,
                                               frame->color_range,
                                               frame->color_primaries,
                                               frame->color_trc,
                                               frame->colorspace,
                                               frame->chroma_location);
        }
    }
    return colorspace;
}

static SDL_PropertiesID CreateVideoTextureProperties(AVFrame *frame, SDL_PixelFormat format, int access)
{
    AVFrameSideData *pSideData;
    SDL_PropertiesID props;
    int width = frame->width;
    int height = frame->height;
    SDL_Colorspace colorspace = GetFrameColorspace(frame);

    /* ITU-R BT.2408-6 recommends using an SDR white point of 203 nits, which is more likely for game content */
    static const float k_flSDRWhitePoint = 203.0f;
    float flMaxLuminance = k_flSDRWhitePoint;

    if (frame->hw_frames_ctx) {
        AVHWFramesContext *frames = (AVHWFramesContext *)(frame->hw_frames_ctx->data);

        width = frames->width;
        height = frames->height;
        if (format == SDL_PIXELFORMAT_UNKNOWN) {
            format = GetTextureFormat(frames->sw_format);
        }
    } else {
        if (format == SDL_PIXELFORMAT_UNKNOWN) {
            format = GetTextureFormat((AVPixelFormat)frame->format);
        }
    }

    props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
    pSideData = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (pSideData) {
        AVMasteringDisplayMetadata *pMasteringDisplayMetadata = (AVMasteringDisplayMetadata *)pSideData->data;
        flMaxLuminance = (float)pMasteringDisplayMetadata->max_luminance.num / pMasteringDisplayMetadata->max_luminance.den;
    } else if (SDL_COLORSPACETRANSFER(colorspace) == SDL_TRANSFER_CHARACTERISTICS_PQ) {
        /* The official definition is 10000, but PQ game content is often mastered for 400 or 1000 nits */
        flMaxLuminance = 1000.0f;
    }
    if (flMaxLuminance > k_flSDRWhitePoint) {
        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, k_flSDRWhitePoint);
        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT, flMaxLuminance / k_flSDRWhitePoint);
    }
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, access);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);

    return props;
}

static SDL_Texture *CreateVulkanVideoTexturePixFmtVulkan(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer, SDL_PropertiesID props)
{
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame->hw_frames_ctx->data);
    AVVulkanFramesContext *vk = (AVVulkanFramesContext *)(frames->hwctx);
    AVVkFrame *pVkFrame = (AVVkFrame *)frame->data[0];
    Uint32 format;

    switch (vk->format[0]) {
    case VK_FORMAT_G8B8G8R8_422_UNORM:
        format = SDL_PIXELFORMAT_YUY2;
        break;
    case VK_FORMAT_B8G8R8G8_422_UNORM:
        format = SDL_PIXELFORMAT_UYVY;
        break;
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        format = SDL_PIXELFORMAT_IYUV;
        break;
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        format = SDL_PIXELFORMAT_NV12;
        break;
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        format = SDL_PIXELFORMAT_P010;
        break;
    default:
        format = SDL_PIXELFORMAT_UNKNOWN;
        break;
    }
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER, (Sint64)pVkFrame->img[0]);
    return SDL_CreateTextureWithProperties(renderer, props);
}

SDL_Texture *CreateVulkanVideoTexture(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer, SDL_PropertiesID props)
{
    if (frame->format == AV_PIX_FMT_VULKAN) {
        return CreateVulkanVideoTexturePixFmtVulkan(context, frame, renderer, props);
    } else {
        SDL_SetError("Unknown hardware frame format");
        return NULL;
    }
}

bool Video::GetTextureForVulkanFrame(AVFrame *frame, SDL_Texture **texture) {
    SDL_PropertiesID props;
    float textureWidth = 0.0f;
    float textureHeight = 0.0f;

    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }

    props = CreateVideoTextureProperties(frame, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_STATIC);
    m_texture = CreateVulkanVideoTexture(vulkan_context, frame, renderer, props);
    SDL_DestroyProperties(props);
    if (!m_texture) {
        return false;
    }

    if (SDL_GetTextureSize(m_texture, &textureWidth, &textureHeight)) {
        m_textureWidth = static_cast<int>(textureWidth);
        m_textureHeight = static_cast<int>(textureHeight);
    }

    if (m_textureWidth <= 0 || m_textureHeight <= 0) {
        AVHWFramesContext *frames = frame->hw_frames_ctx ? (AVHWFramesContext *)frame->hw_frames_ctx->data : nullptr;
        if (frames) {
            m_textureWidth = frames->width;
            m_textureHeight = frames->height;
        } else {
            m_textureWidth = frame->width;
            m_textureHeight = frame->height;
        }
    }

    return true;
}

static bool SupportedPixelFormat(enum AVPixelFormat format)
{


        if (format == AV_PIX_FMT_VULKAN) {
            return true;
        }

    if (GetTextureFormat(format) != SDL_PIXELFORMAT_UNKNOWN) {
        return true;
    }
    return false;
}

static enum AVPixelFormat GetSupportedPixelFormat(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            /* We support all memory formats using swscale */
            break;
        }

        if (SupportedPixelFormat(*p)) {
            /* We support this format */
            break;
        }
    }

    if (*p == AV_PIX_FMT_NONE) {
        SDL_Log("Couldn't find a supported pixel format:");
        for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            SDL_Log("    %s", av_get_pix_fmt_name(*p));
        }
    }

    return *p;
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
static void AddQueueFamily(AVVulkanDeviceContext *ctx, int idx, int num, VkQueueFlagBits flags)
{
    AVVulkanDeviceQueueFamily *entry = &ctx->qf[ctx->nb_qf++];
    entry->idx = idx;
    entry->num = num;
    entry->flags = flags;
}
#endif /* LIBAVUTIL_VERSION_INT */

void SetupVulkanDeviceContextData(VulkanVideoContext *context, AVVulkanDeviceContext *ctx)
{
    ctx->get_proc_addr = context->vkGetInstanceProcAddr;
    ctx->inst = context->instance;
    ctx->phys_dev = context->physicalDevice;
    ctx->act_dev = context->device;
    ctx->device_features = context->features.device_features;
    ctx->enabled_inst_extensions = context->instanceExtensions;
    ctx->nb_enabled_inst_extensions = context->instanceExtensionsCount;
    ctx->enabled_dev_extensions = context->deviceExtensions;
    ctx->nb_enabled_dev_extensions = context->deviceExtensionsCount;
    AddQueueFamily(ctx, context->graphicsQueueFamilyIndex, context->graphicsQueueCount, VK_QUEUE_GRAPHICS_BIT);
    AddQueueFamily(ctx, context->transferQueueFamilyIndex, context->transferQueueCount, VK_QUEUE_TRANSFER_BIT);
    AddQueueFamily(ctx, context->computeQueueFamilyIndex, context->computeQueueCount, VK_QUEUE_COMPUTE_BIT);
    AddQueueFamily(ctx, context->decodeQueueFamilyIndex, context->decodeQueueCount, VK_QUEUE_VIDEO_DECODE_BIT_KHR);
}

AVCodecContext *Video::OpenVideoStream(AVFormatContext *ic, int stream, const AVCodec *codec) { 
    AVStream *st = ic->streams[stream];
    AVCodecParameters *codecpar = st->codecpar;
    AVCodecContext *context;
    const AVCodecHWConfig *config;
    int i;
    int result;

    SDL_Log("Video stream: %s %dx%d", avcodec_get_name(codec->id), codecpar->width, codecpar->height);

    context = avcodec_alloc_context3(NULL);
    if (!context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "avcodec_alloc_context3 failed");
        return NULL;
    }

    result = avcodec_parameters_to_context(context, ic->streams[stream]->codecpar);
    if (result < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "avcodec_parameters_to_context failed: %s", av_err2str(result));
        avcodec_free_context(&context);
        return NULL;
    }
    context->pkt_timebase = ic->streams[stream]->time_base;

    /* Look for supported hardware accelerated configurations */
    i = 0;
    while (!context->hw_device_ctx &&
           (config = avcodec_get_hw_config(codec, i++)) != NULL) {
#if 0
        SDL_Log("Found %s hardware acceleration with pixel format %s", av_hwdevice_get_type_name(config->device_type), av_get_pix_fmt_name(config->pix_fmt));
#endif

        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
            !SupportedPixelFormat(config->pix_fmt)) {
            continue;
        }

#ifdef SDL_PLATFORM_WIN32
        if (d3d11_device && config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
            AVD3D11VADeviceContext *device_context;

            context->hw_device_ctx = av_hwdevice_ctx_alloc(config->device_type);

            device_context = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)context->hw_device_ctx->data)->hwctx;
            device_context->device = d3d11_device;
            ID3D11Device_AddRef(device_context->device);
            device_context->device_context = d3d11_context;
            ID3D11DeviceContext_AddRef(device_context->device_context);

            result = av_hwdevice_ctx_init(context->hw_device_ctx);
            if (result < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create %s hardware device context: %s", av_hwdevice_get_type_name(config->device_type), av_err2str(result));
            } else {
                SDL_Log("Using %s hardware acceleration with pixel format %s", av_hwdevice_get_type_name(config->device_type), av_get_pix_fmt_name(config->pix_fmt));
            }
        } else
#endif
        if (vulkan_context && config->device_type == AV_HWDEVICE_TYPE_VULKAN) {
            AVVulkanDeviceContext *device_context;

            context->hw_device_ctx = av_hwdevice_ctx_alloc(config->device_type);

            device_context = (AVVulkanDeviceContext *)((AVHWDeviceContext *)context->hw_device_ctx->data)->hwctx;
            SetupVulkanDeviceContextData(vulkan_context, device_context);

            result = av_hwdevice_ctx_init(context->hw_device_ctx);
            if (result < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create %s hardware device context: %s", av_hwdevice_get_type_name(config->device_type), av_err2str(result));
            } else {
                SDL_Log("Using %s hardware acceleration with pixel format %s", av_hwdevice_get_type_name(config->device_type), av_get_pix_fmt_name(config->pix_fmt));
            }
        } else {
            result = av_hwdevice_ctx_create(&context->hw_device_ctx, config->device_type, NULL, NULL, 0);
            if (result < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create %s hardware device context: %s", av_hwdevice_get_type_name(config->device_type), av_err2str(result));
            } else {
                SDL_Log("Using %s hardware acceleration with pixel format %s", av_hwdevice_get_type_name(config->device_type), av_get_pix_fmt_name(config->pix_fmt));
            }
        }
    }

    /* Allow supported hardware accelerated pixel formats */
    context->get_format = GetSupportedPixelFormat;

    if (codecpar->codec_id == AV_CODEC_ID_VVC) {
        context->strict_std_compliance = -2;

        /* Enable threaded decoding, VVC decode is slow */
        context->thread_count = SDL_GetNumLogicalCPUCores();
        context->thread_type = (FF_THREAD_FRAME | FF_THREAD_SLICE);
    }

    result = avcodec_open2(context, codec, NULL);
    if (result < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't open codec %s: %s", avcodec_get_name(context->codec_id), av_err2str(result));
        avcodec_free_context(&context);
        return NULL;
    }

    return context;
 }

 void Video::decodeLoop() {
    if (!ensureDecoder(renderer)) {
        return;
    }
    
    if (!flushing) {
        result = av_read_frame(m_formatContext, m_packet);
        if (result < 0) {
            SDL_Log("End of stream, finishing decode");
            if (m_codecContext) {
                avcodec_flush_buffers(m_codecContext);
            }
            flushing = true;
        } else {
            if (m_packet->stream_index == m_videoStreamIndex) {
                result = avcodec_send_packet(m_codecContext, m_packet);
                if (result < 0) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "avcodec_send_packet(m_codecContext) failed: %s", av_err2str(result));
                }
            }
            av_packet_unref(m_packet);
        }
    }
    decoded = false;
    if (m_codecContext) {
        while (avcodec_receive_frame(m_codecContext, m_frame) >= 0) {
            double pts = ((double)m_frame->pts * m_codecContext->pkt_timebase.num) / m_codecContext->pkt_timebase.den;
            if (first_pts < 0.0) {
                first_pts = pts;
            }
            pts -= first_pts;

            HandleVideoFrame(m_frame, pts);
            decoded = true;
        }
    }
}

void Video::HandleVideoFrame(AVFrame *frame, double pts) {
    /* Quick and dirty PTS handling */
    if (!video_start) {
        video_start = SDL_GetTicks();
    }
    double now = (double)(SDL_GetTicks() - video_start) / 1000.0;
    if (now < pts) {
        SDL_DelayPrecise((Uint64)((pts - now) * SDL_NS_PER_SECOND));
    }

    DisplayVideoFrame(frame);
}

void Video::DisplayVideoFrame(AVFrame *frame) {
    DisplayVideoTexture(frame);
}

void Video::DisplayVideoTexture(AVFrame *frame) {
    if (!GetTextureForFrame(frame, &m_texture)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't get texture for frame: %s", SDL_GetError());
        return;
    }

    if (BeginFrameRendering(frame) < 0) {
        return;
    }

    FinishFrameRendering(frame);
}

bool Video::GetTextureForFrame(AVFrame *frame, SDL_Texture **texture)
{
    return GetTextureForVulkanFrame(frame, texture);
}

int Video::BeginFrameRendering(AVFrame *frame)
{
    if (frame->format == AV_PIX_FMT_VULKAN) {
        return BeginVulkanFrameRendering(vulkan_context, frame, renderer);
    }
    return 0;
}

int Video::BeginVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer)
{
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame->hw_frames_ctx->data);
    AVVulkanFramesContext *vk = (AVVulkanFramesContext *)(frames->hwctx);
    AVVkFrame *pVkFrame = (AVVkFrame *)frame->data[0];

    if (CreateCommandBuffers(context, renderer) < 0) {
        return -1;
    }

    vk->lock_frame(frames, pVkFrame);

    VkTimelineSemaphoreSubmitInfo timeline;
    SDL_zero(timeline);
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = pVkFrame->sem_value;

    VkPipelineStageFlags pipelineStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submitInfo;
    SDL_zero(submitInfo);
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = pVkFrame->sem;
    submitInfo.pWaitDstStageMask = &pipelineStageMask;
    submitInfo.pNext = &timeline;

    if (pVkFrame->layout[0] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkCommandBuffer commandBuffer = context->commandBuffers[context->commandBufferIndex];

        VkCommandBufferBeginInfo beginInfo;
        SDL_zero(beginInfo);
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        context->vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkImageMemoryBarrier2 barrier;
        SDL_zero(barrier);
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = pVkFrame->layout[0];
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = pVkFrame->img[0];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcQueueFamilyIndex = pVkFrame->queue_family[0];
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

        VkDependencyInfo dep;
        SDL_zero(dep);
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        context->vkCmdPipelineBarrier2(commandBuffer, &dep);

        context->vkEndCommandBuffer(commandBuffer);

        // Add the image barrier to the submit info
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &context->commandBuffers[context->commandBufferIndex];

        pVkFrame->layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pVkFrame->queue_family[0] = VK_QUEUE_FAMILY_IGNORED;
    }

    VkResult result = context->vkQueueSubmit(context->graphicsQueue, 1, &submitInfo, 0);
    if (result != VK_SUCCESS) {
        // Don't return an error here, we need to complete the frame operation
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION , "vkQueueSubmit(): %s", getVulkanResultString(result));
    }

    return 0;
}

int Video::CreateCommandBuffers(VulkanVideoContext *context, SDL_Renderer *renderer)
{
    uint32_t commandBufferCount = (uint32_t)SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_VULKAN_SWAPCHAIN_IMAGE_COUNT_NUMBER, 1);

    if (commandBufferCount > context->commandBufferCount) {
        uint32_t needed = (commandBufferCount - context->commandBufferCount);
        VkCommandBuffer *commandBuffers = (VkCommandBuffer *)SDL_realloc(context->commandBuffers, commandBufferCount * sizeof(*commandBuffers));
        if (!commandBuffers) {
            return -1;
        }
        context->commandBuffers = commandBuffers;

        VkCommandBufferAllocateInfo commandBufferAllocateInfo;
        SDL_zero(commandBufferAllocateInfo);
        commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferAllocateInfo.commandPool = context->commandPool;
        commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferAllocateInfo.commandBufferCount = needed;
        VkResult result = context->vkAllocateCommandBuffers(context->device, &commandBufferAllocateInfo, &context->commandBuffers[context->commandBufferCount]);
        if (result != VK_SUCCESS) {
            SDL_SetError("vkAllocateCommandBuffers(): %s", getVulkanResultString(result));
            return -1;
        }

        context->commandBufferCount = commandBufferCount;
    }
    return 0;
}

int Video::FinishFrameRendering(AVFrame *frame) {
    if (frame->format == AV_PIX_FMT_VULKAN) {
        return FinishVulkanFrameRendering(vulkan_context, frame, renderer);
    }
    return 0;
}

int Video::FinishVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer) {
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame->hw_frames_ctx->data);
    AVVulkanFramesContext *vk = (AVVulkanFramesContext *)(frames->hwctx);
    AVVkFrame *pVkFrame = (AVVkFrame *)frame->data[0];

    // Transition the frame back to ffmpeg
    ++pVkFrame->sem_value[0];

    VkTimelineSemaphoreSubmitInfo timeline;
    SDL_zero(timeline);
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = pVkFrame->sem_value;

    VkSubmitInfo submitInfo;
    SDL_zero(submitInfo);
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = pVkFrame->sem;
    submitInfo.pNext = &timeline;

    VkResult result = context->vkQueueSubmit(context->graphicsQueue, 1, &submitInfo, 0);
    if (result != VK_SUCCESS) {
        // Don't return an error here, we need to complete the frame operation
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "vkQueueSubmit(): %s", getVulkanResultString(result));
    }

    vk->unlock_frame(frames, pVkFrame);

    context->commandBufferIndex = (context->commandBufferIndex + 1) % context->commandBufferCount;

    return 0;
}