#include "screen_renderer.hpp"

static float vertexData[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f
};

static inline VkDeviceSize aligned(VkDeviceSize v, VkDeviceSize byteAlign) {
    return (v + byteAlign - 1) & ~(byteAlign - 1);
}

ScreenRenderer::ScreenRenderer(QVulkanWindow *w) : m_window(w) {}

VkShaderModule ScreenRenderer::createShader(const QString &name) {
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("Failed to read shader %s", qPrintable(name));
        return VK_NULL_HANDLE;
    }
    QByteArray blob = file.readAll();
    file.close();

    VkShaderModuleCreateInfo shaderInfo;
    memset(&shaderInfo, 0, sizeof(shaderInfo));
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = blob.size();
    shaderInfo.pCode = reinterpret_cast<const uint32_t *>(blob.constData());
    VkShaderModule shaderModule;
    VkResult err = m_devFuncs->vkCreateShaderModule(m_window->device(), &shaderInfo, nullptr, &shaderModule);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create shader module: %d", err);
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

void ScreenRenderer::initResources() {
    qDebug("initResources");

    VkDevice dev = m_window->device();
    m_devFuncs = m_window->vulkanInstance()->deviceFunctions(dev);

    QList<QVulkanExtension> extensions = m_window->supportedDeviceExtensions();
    qDebug() << "Supported device extensions:" << extensions.size();
    for (const QVulkanExtension &ext : extensions) {
        qDebug() << "  -" << ext.name << "version" << ext.version;
    }

    VkBufferCreateInfo bufInfo;
    memset(&bufInfo, 0, sizeof(bufInfo));
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = sizeof(vertexData);
    bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VkResult err = m_devFuncs->vkCreateBuffer(dev, &bufInfo, nullptr, &m_buf);
    if (err != VK_SUCCESS) {
        qFatal("Failed to create buffer: %d", err);
    }

    VkMemoryRequirements memReq;
    m_devFuncs->vkGetBufferMemoryRequirements(dev, m_buf, &memReq);

    VkMemoryAllocateInfo memAllocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        memReq.size,
        m_window->hostVisibleMemoryIndex()
    };

    err = m_devFuncs->vkAllocateMemory(dev, &memAllocInfo, nullptr, &m_bufMem);
    if (err != VK_SUCCESS) {
        qFatal("Failed to allocate memory: %d", err);
    }

    err = m_devFuncs->vkBindBufferMemory(dev, m_buf, m_bufMem, 0);
    if (err != VK_SUCCESS) {
        qFatal("Failed to bind buffer memory: %d", err);
    }

    quint8 *p;
    err = m_devFuncs->vkMapMemory(dev, m_bufMem, 0, memReq.size, 0, reinterpret_cast<void **>(&p));
    if (err != VK_SUCCESS)
        qFatal("Failed to map memory: %d", err);
    memcpy(p, vertexData, sizeof(vertexData));
    m_devFuncs->vkUnmapMemory(dev, m_bufMem);

    VkVertexInputBindingDescription vertexBindingDesc = {
        0, // binding
        4 * sizeof(float),  // pos(2) + uv(2)
        VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertexAttrDesc[] = {
        { // position (location 0)
            0, // location
            0, // binding
            VK_FORMAT_R32G32_SFLOAT,
            0
        },
        { // uv (location 1)
            1,
            0,
            VK_FORMAT_R32G32_SFLOAT,
            2 * sizeof(float)
        }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo;
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.pNext = nullptr;
    vertexInputInfo.flags = 0;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &vertexBindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = vertexAttrDesc;

    VkPipelineCacheCreateInfo pipelineCacheInfo;
    memset(&pipelineCacheInfo, 0, sizeof(pipelineCacheInfo));
    pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    err = m_devFuncs->vkCreatePipelineCache(dev, &pipelineCacheInfo, nullptr, &m_pipelineCache);
    if (err != VK_SUCCESS) {
        qFatal("Failed to create pipeline cache: %d", err);
    }
    qDebug("Pipeline creation deferred until video descriptor is available");
}

void ScreenRenderer::initSwapChainResources() {
    qDebug("initSwapChainResources");
}

void ScreenRenderer::releaseSwapChainResources() {
    qDebug("releaseSwapChainResources");
}

void ScreenRenderer::releaseResources() {
    qDebug("releaseResources");

    VkDevice dev = m_window->device();

    if (m_pipeline) {
        m_devFuncs->vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    if (m_pipelineLayout) {
        m_devFuncs->vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_pipelineCache) {
        m_devFuncs->vkDestroyPipelineCache(dev, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }

    if (m_descSetLayout) {
        m_devFuncs->vkDestroyDescriptorSetLayout(dev, m_descSetLayout, nullptr);
        m_descSetLayout = VK_NULL_HANDLE;
    }

    if (m_descPool) {
        m_devFuncs->vkDestroyDescriptorPool(dev, m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }

    if (m_buf) {
        m_devFuncs->vkDestroyBuffer(dev, m_buf, nullptr);
        m_buf = VK_NULL_HANDLE;
    }

    if (m_bufMem) {
        m_devFuncs->vkFreeMemory(dev, m_bufMem, nullptr);
        m_bufMem = VK_NULL_HANDLE;
    }

    cleanupCudaInterop();
    
    releaseVideoImage();
    
    if (m_currentVideoFrame) {
        av_frame_free(&m_currentVideoFrame);
        m_currentVideoFrame = nullptr;
    }
}

void ScreenRenderer::startNextFrame() {
    VkDevice dev = m_window->device();
    VkCommandBuffer cb = m_window->currentCommandBuffer();
    const QSize sz = m_window->swapChainImageSize();

    VkClearColorValue clearColor = {{ 0, 0, 0, 1 }};
    VkClearDepthStencilValue clearDS = { 1, 0 };
    VkClearValue clearValues[3];
    memset(clearValues, 0, sizeof(clearValues));
    clearValues[0].color = clearValues[2].color = clearColor;
    clearValues[1].depthStencil = clearDS;

    VkRenderPassBeginInfo rpBeginInfo;
    memset(&rpBeginInfo, 0, sizeof(rpBeginInfo));
    rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBeginInfo.renderPass = m_window->defaultRenderPass();
    rpBeginInfo.framebuffer = m_window->currentFramebuffer();
    rpBeginInfo.renderArea.extent.width = sz.width();
    rpBeginInfo.renderArea.extent.height = sz.height();
    rpBeginInfo.clearValueCount = m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    rpBeginInfo.pClearValues = clearValues;
    VkCommandBuffer cmdBuf = m_window->currentCommandBuffer();
    m_devFuncs->vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (m_hasVideoFrame && m_pendingFrameUpdate) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentVideoFrame) {
            updateVideoImage(m_currentVideoFrame);
            m_pendingFrameUpdate = false;
            if (!m_videoImageCreated) {
                m_videoImageCreated = true;
            }
        }
    }
    
    if (m_pipeline == VK_NULL_HANDLE) {
        m_devFuncs->vkCmdEndRenderPass(cmdBuf);
        m_window->frameReady();
        return;
    }
    
    m_devFuncs->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    
    // Bind image descriptor set if video is available
    if (m_imageDescSet && m_hasVideoFrame) {
        m_devFuncs->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_imageDescSet, 0, nullptr);
    }
    
    VkDeviceSize vbOffset = 0;
    m_devFuncs->vkCmdBindVertexBuffers(cb, 0, 1, &m_buf, &vbOffset);

    VkViewport viewport;
    if (m_hasVideoFrame && m_videoWidth > 0 && m_videoHeight > 0) {
        float videoAspect = static_cast<float>(m_videoWidth) / static_cast<float>(m_videoHeight);
        float windowAspect = static_cast<float>(sz.width()) / static_cast<float>(sz.height());

        float vpWidth, vpHeight, vpX, vpY;
        if (windowAspect > videoAspect) {
            vpHeight = static_cast<float>(sz.height());
            vpWidth = vpHeight * videoAspect;
            vpX = (static_cast<float>(sz.width()) - vpWidth) / 2.0f;
            vpY = 0.0f;
        } else {
            vpWidth = static_cast<float>(sz.width());
            vpHeight = vpWidth / videoAspect;
            vpX = 0.0f;
            vpY = (static_cast<float>(sz.height()) - vpHeight) / 2.0f;
        }

        viewport.x = vpX;
        viewport.y = vpY;
        viewport.width = vpWidth;
        viewport.height = vpHeight;
    } else {
        viewport.x = viewport.y = 0;
        viewport.width = sz.width();
        viewport.height = sz.height();
    }
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    m_devFuncs->vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor;
    scissor.offset.x = static_cast<int32_t>(viewport.x);
    scissor.offset.y = static_cast<int32_t>(viewport.y);
    scissor.extent.width = static_cast<uint32_t>(viewport.width);
    scissor.extent.height = static_cast<uint32_t>(viewport.height);
    m_devFuncs->vkCmdSetScissor(cb, 0, 1, &scissor);
    m_devFuncs->vkCmdDraw(cb, 4, 1, 0, 0);
    m_devFuncs->vkCmdEndRenderPass(cmdBuf);

    m_window->frameReady();
}

void ScreenRenderer::receiveDecodedFrame(AVFrame *frame) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    
    if (m_currentVideoFrame) {
        av_frame_free(&m_currentVideoFrame);
    }
    
    m_currentVideoFrame = av_frame_clone(frame);
    
    if (m_currentVideoFrame) {
        m_hasVideoFrame = true;
        m_pendingFrameUpdate = true;
        m_videoWidth = m_currentVideoFrame->width;
        m_videoHeight = m_currentVideoFrame->height;
        requestFrameUpdate();
    }
}

void ScreenRenderer::createVideoImage(uint32_t width, uint32_t height) {
    VkDevice dev = m_window->device();
    
    releaseVideoImage();
    
    m_videoWidth = width;
    m_videoHeight = height;
    
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkResult err = m_devFuncs->vkCreateImage(dev, &imageInfo, nullptr, &m_videoImage);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create video image: %d", err);
        return;
    }
    
    VkMemoryRequirements memReq;
    m_devFuncs->vkGetImageMemoryRequirements(dev, m_videoImage, &memReq);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    
    VkPhysicalDeviceMemoryProperties memProps;
    m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(m_window->physicalDevice(), &memProps);
    
    uint32_t memTypeIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    allocInfo.memoryTypeIndex = memTypeIndex;
    
    err = m_devFuncs->vkAllocateMemory(dev, &allocInfo, nullptr, &m_videoImageMemory);
    if (err != VK_SUCCESS) {
        qWarning("Failed to allocate video image memory: %d", err);
        return;
    }
    
    m_devFuncs->vkBindImageMemory(dev, m_videoImage, m_videoImageMemory, 0);
    
    // Create image view for NV12 (2-plane YCbCr)
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_videoImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    err = m_devFuncs->vkCreateImageView(dev, &viewInfo, nullptr, &m_videoImageView);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create video image view: %d", err);
        return;
    }
    
    // Create sampler with YCbCr conversion
    VkSamplerYcbcrConversionCreateInfo conversionInfo = {};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversionInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    conversionInfo.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    conversionInfo.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    conversionInfo.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };
    conversionInfo.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversionInfo.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversionInfo.chromaFilter = VK_FILTER_LINEAR;
    conversionInfo.forceExplicitReconstruction = VK_FALSE;
    
    VkSamplerYcbcrConversion ycbcrConversion;
    err = m_devFuncs->vkCreateSamplerYcbcrConversion(dev, &conversionInfo, nullptr, &ycbcrConversion);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create YCbCr conversion: %d", err);
        return;
    }
    
    VkSamplerYcbcrConversionInfo ycbcrInfo = {};
    ycbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcrInfo.conversion = ycbcrConversion;
    
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &ycbcrInfo;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    
    err = m_devFuncs->vkCreateSampler(dev, &samplerInfo, nullptr, &m_videoSampler);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create video sampler: %d", err);
        return;
    }
    
    std::println("Created video image: {}x{} with NV12 format", width, height);
}

void ScreenRenderer::releaseVideoImage() {
    VkDevice dev = m_window->device();
    
    if (m_imageDescSet) {
        m_devFuncs->vkFreeDescriptorSets(dev, m_imageDescPool, 1, &m_imageDescSet);
        m_imageDescSet = VK_NULL_HANDLE;
    }
    
    if (m_imageDescSetLayout) {
        m_devFuncs->vkDestroyDescriptorSetLayout(dev, m_imageDescSetLayout, nullptr);
        m_imageDescSetLayout = VK_NULL_HANDLE;
    }
    
    if (m_imageDescPool) {
        m_devFuncs->vkDestroyDescriptorPool(dev, m_imageDescPool, nullptr);
        m_imageDescPool = VK_NULL_HANDLE;
    }
    
    if (m_videoSampler) {
        m_devFuncs->vkDestroySampler(dev, m_videoSampler, nullptr);
        m_videoSampler = VK_NULL_HANDLE;
    }
    
    if (m_ycbcrConversion) {
        m_devFuncs->vkDestroySamplerYcbcrConversion(dev, m_ycbcrConversion, nullptr);
        m_ycbcrConversion = VK_NULL_HANDLE;
    }
    
    if (m_videoImageView) {
        m_devFuncs->vkDestroyImageView(dev, m_videoImageView, nullptr);
        m_videoImageView = VK_NULL_HANDLE;
    }
    
    if (m_videoImageOwned) {
        if (m_videoImage) {
            m_devFuncs->vkDestroyImage(dev, m_videoImage, nullptr);
            m_videoImage = VK_NULL_HANDLE;
        }

        if (m_videoImageMemory) {
            m_devFuncs->vkFreeMemory(dev, m_videoImageMemory, nullptr);
            m_videoImageMemory = VK_NULL_HANDLE;
        }
    }

    m_videoImage = VK_NULL_HANDLE;
    m_videoImageMemory = VK_NULL_HANDLE;
    m_videoImageOwned = true;
    m_videoFormat = VK_FORMAT_UNDEFINED;
}

bool ScreenRenderer::ensureVideoResources(uint32_t width, uint32_t height) {
    if (m_videoImage && (m_videoWidth != width || m_videoHeight != height)) {
        releaseVideoImage();
    }

    m_videoWidth = width;
    m_videoHeight = height;

    if (m_videoWidth == 0 || m_videoHeight == 0) {
        std::println("Invalid frame dimensions: {}x{}", m_videoWidth, m_videoHeight);
        return false;
    }

    if (m_videoImage) {
        return true;
    }

    VkDevice dev = m_window->device();
    m_videoImageOwned = true;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    imageInfo.extent.width = m_videoWidth;
    imageInfo.extent.height = m_videoHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult err = m_devFuncs->vkCreateImage(dev, &imageInfo, nullptr, &m_videoImage);
    if (err != VK_SUCCESS) {
        std::println("Failed to create video image: {}", static_cast<int>(err));
        return false;
    }

    VkMemoryRequirements memReqs;
    m_devFuncs->vkGetImageMemoryRequirements(dev, m_videoImage, &memReqs);

    VkMemoryAllocateInfo memAllocInfo = {};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReqs.size;

    VkPhysicalDeviceMemoryProperties memProps;
    m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(
        m_window->physicalDevice(), &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memAllocInfo.memoryTypeIndex = i;
            break;
        }
    }

    err = m_devFuncs->vkAllocateMemory(dev, &memAllocInfo, nullptr, &m_videoImageMemory);
    if (err != VK_SUCCESS) {
        std::println("Failed to allocate video image memory: {}", static_cast<int>(err));
        m_devFuncs->vkDestroyImage(dev, m_videoImage, nullptr);
        m_videoImage = VK_NULL_HANDLE;
        return false;
    }

    err = m_devFuncs->vkBindImageMemory(dev, m_videoImage, m_videoImageMemory, 0);
    if (err != VK_SUCCESS) {
        std::println("Failed to bind image memory: {}", static_cast<int>(err));
        return false;
    }

    VkSamplerYcbcrConversionCreateInfo conversionInfo = {};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversionInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    conversionInfo.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    conversionInfo.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    conversionInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    conversionInfo.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversionInfo.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversionInfo.chromaFilter = VK_FILTER_LINEAR;
    conversionInfo.forceExplicitReconstruction = VK_FALSE;

    err = m_devFuncs->vkCreateSamplerYcbcrConversion(dev, &conversionInfo, nullptr, &m_ycbcrConversion);
    if (err != VK_SUCCESS) {
        std::println("Failed to create YCbCr conversion: {}", static_cast<int>(err));
        return false;
    }

    VkSamplerYcbcrConversionInfo ycbcrInfo = {};
    ycbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcrInfo.conversion = m_ycbcrConversion;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = &ycbcrInfo;
    viewInfo.image = m_videoImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    err = m_devFuncs->vkCreateImageView(dev, &viewInfo, nullptr, &m_videoImageView);
    if (err != VK_SUCCESS) {
        std::println("Failed to create image view: {}", static_cast<int>(err));
        return false;
    }

    VkSamplerYcbcrConversionInfo samplerYcbcrInfo = {};
    samplerYcbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    samplerYcbcrInfo.conversion = m_ycbcrConversion;

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &samplerYcbcrInfo;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    err = m_devFuncs->vkCreateSampler(dev, &samplerInfo, nullptr, &m_videoSampler);
    if (err != VK_SUCCESS) {
        std::println("Failed to create sampler: {}", static_cast<int>(err));
        return false;
    }

    transitionImageLayout(m_videoImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_videoImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_videoFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    createImageDescriptorSet();

    std::println("Created video image: {}x{}", m_videoWidth, m_videoHeight);
    return true;
}


void ScreenRenderer::updateVideoImage(AVFrame *frame) {
    if (!frame) {
        std::println("Invalid frame");
        return;
    }

    if (!m_devFuncs) {
        std::println("Renderer not initialized yet, skipping frame");
        return;
    }
    
    VkDevice dev = m_window->device();
    if (!dev) {
        std::println("No Vulkan device available");
        return;
    }
    

    if (frame->format == AV_PIX_FMT_CUDA) {
        if (!ensureVideoResources(frame->width, frame->height)) {
            return;
        }

        std::println("Processing CUDA frame {}x{}", frame->width, frame->height);
        std::println("Using CUDA→Vulkan device copy path");

        if (!frame->hw_frames_ctx || !frame->hw_frames_ctx->data) {
            std::println("Frame missing hw_frames_ctx, cannot use CUDA interop");
            return;
        }

        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
        if (!framesCtx || !framesCtx->device_ctx || !framesCtx->device_ctx->hwctx) {
            std::println("Invalid CUDA device context on frame");
            return;
        }

        auto *cudaCtx = static_cast<AVCUDADeviceContext *>(framesCtx->device_ctx->hwctx);
        CUcontext cuCtx = cudaCtx->cuda_ctx;

        if (!ensureCudaInterop(frame, dev, cuCtx)) {
            std::println("Failed to prepare CUDA interop resources");
            return;
        }

        VkDeviceSize yPlaneSize = static_cast<VkDeviceSize>(m_videoWidth) * m_videoHeight;

        m_cu->cuCtxPushCurrent(cuCtx);

        if (!m_cudaStream) {
            CUresult streamRes = m_cu->cuStreamCreate(&m_cudaStream, CU_STREAM_NON_BLOCKING);
            if (streamRes != CUDA_SUCCESS) {
                std::println("cuStreamCreate failed: {}", static_cast<int>(streamRes));
                m_cu->cuCtxPopCurrent(nullptr);
                return;
            }
        }

        if (m_cudaStagingPtr == 0) {
            std::println("CUDA staging pointer is null after interop mapping");
            m_cu->cuCtxPopCurrent(nullptr);
            return;
        }

        CUDA_MEMCPY2D copyY = {};
        copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copyY.srcDevice = reinterpret_cast<CUdeviceptr>(frame->data[0]);
        copyY.srcPitch = frame->linesize[0];
        copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copyY.dstDevice = m_cudaStagingPtr;
        copyY.dstPitch = m_videoWidth;
        copyY.WidthInBytes = m_videoWidth;
        copyY.Height = m_videoHeight;

        CUresult cuRes = m_cu->cuMemcpy2DAsync(&copyY, m_cudaStream);
        if (cuRes != CUDA_SUCCESS) {
            std::println("cuMemcpy2DAsync(Y) failed: {}", static_cast<int>(cuRes));
            m_cu->cuCtxPopCurrent(nullptr);
            return;
        }

        CUDA_MEMCPY2D copyUV = {};
        copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copyUV.srcDevice = reinterpret_cast<CUdeviceptr>(frame->data[1]);
        copyUV.srcPitch = frame->linesize[1];
        copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copyUV.dstDevice = m_cudaStagingPtr + yPlaneSize;
        copyUV.dstPitch = m_videoWidth;
        copyUV.WidthInBytes = m_videoWidth;
        copyUV.Height = m_videoHeight / 2;

        cuRes = m_cu->cuMemcpy2DAsync(&copyUV, m_cudaStream);
        if (cuRes != CUDA_SUCCESS) {
            std::println("cuMemcpy2DAsync(UV) failed: {}", static_cast<int>(cuRes));
            m_cu->cuCtxPopCurrent(nullptr);
            return;
        }

        cuRes = m_cu->cuStreamSynchronize(m_cudaStream);
        if (cuRes != CUDA_SUCCESS) {
            std::println("cuStreamSynchronize failed: {}", static_cast<int>(cuRes));
            m_cu->cuCtxPopCurrent(nullptr);
            return;
        }

        m_cu->cuCtxPopCurrent(nullptr);

        transitionImageLayout(m_videoImage, m_videoImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkCommandBufferAllocateInfo cmdAllocInfo = {};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandPool = m_window->graphicsCommandPool();
        cmdAllocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        m_devFuncs->vkAllocateCommandBuffers(dev, &cmdAllocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_devFuncs->vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkBufferImageCopy yRegion = {};
        yRegion.bufferOffset = 0;
        yRegion.bufferRowLength = 0;
        yRegion.bufferImageHeight = 0;
        yRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        yRegion.imageSubresource.mipLevel = 0;
        yRegion.imageSubresource.baseArrayLayer = 0;
        yRegion.imageSubresource.layerCount = 1;
        yRegion.imageOffset = {0, 0, 0};
        yRegion.imageExtent = {m_videoWidth, m_videoHeight, 1};

        m_devFuncs->vkCmdCopyBufferToImage(commandBuffer, m_cudaStagingBuffer, m_videoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &yRegion);

        VkBufferImageCopy uvRegion = {};
        uvRegion.bufferOffset = yPlaneSize;
        uvRegion.bufferRowLength = 0;
        uvRegion.bufferImageHeight = 0;
        uvRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        uvRegion.imageSubresource.mipLevel = 0;
        uvRegion.imageSubresource.baseArrayLayer = 0;
        uvRegion.imageSubresource.layerCount = 1;
        uvRegion.imageOffset = {0, 0, 0};
        uvRegion.imageExtent = {m_videoWidth / 2, m_videoHeight / 2, 1};

        m_devFuncs->vkCmdCopyBufferToImage(commandBuffer, m_cudaStagingBuffer, m_videoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uvRegion);
        m_devFuncs->vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        m_devFuncs->vkQueueSubmit(m_window->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        m_devFuncs->vkQueueWaitIdle(m_window->graphicsQueue());
        m_devFuncs->vkFreeCommandBuffers(dev, m_window->graphicsCommandPool(), 1, &commandBuffer);

        transitionImageLayout(m_videoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_videoImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::println("Successfully uploaded CUDA frame to Vulkan image ({}x{} NV12)", m_videoWidth, m_videoHeight);
        return;
    }

    if (frame->format != AV_PIX_FMT_VULKAN) {
        std::println("Unexpected frame format: {}", frame->format);
        return;
    }

    if (!frame->hw_frames_ctx || !frame->hw_frames_ctx->data || !frame->data[0]) {
        std::println("Vulkan frame missing hw_frames_ctx data");
        return;
    }

    auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
    auto *vkFramesCtx = reinterpret_cast<AVVulkanFramesContext *>(framesCtx->hwctx);
    auto *vkFrame = reinterpret_cast<AVVkFrame *>(frame->data[0]);

    if (!vkFramesCtx || !vkFrame) {
        std::println("Invalid Vulkan frame context");
        return;
    }

    if (vkFramesCtx->lock_frame) {
        vkFramesCtx->lock_frame(framesCtx, vkFrame);
    }

    VkImage image = vkFrame->img[0];
    VkImageLayout layout = vkFrame->layout[0];
    VkFormat format = vkFramesCtx->format[0] ? vkFramesCtx->format[0] : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

    if (!image) {
        std::println("Vulkan frame missing VkImage");
        if (vkFramesCtx->unlock_frame) {
            vkFramesCtx->unlock_frame(framesCtx, vkFrame);
        }
        return;
    }

    if (m_videoImage != image || m_videoFormat != format) {
        releaseVideoImage();
    }

    m_videoImageOwned = false;
    m_videoImage = image;
    m_videoFormat = format;
    m_videoWidth = frame->width;
    m_videoHeight = frame->height;
    m_videoImageLayout = (layout != VK_IMAGE_LAYOUT_UNDEFINED) ? layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (!m_videoImageView) {
        VkSamplerYcbcrConversionCreateInfo conversionInfo = {};
        conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        conversionInfo.format = format;
        conversionInfo.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        conversionInfo.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        conversionInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        conversionInfo.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversionInfo.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversionInfo.chromaFilter = VK_FILTER_LINEAR;
        conversionInfo.forceExplicitReconstruction = VK_FALSE;

        VkResult err = m_devFuncs->vkCreateSamplerYcbcrConversion(dev, &conversionInfo, nullptr, &m_ycbcrConversion);
        if (err != VK_SUCCESS) {
            std::println("Failed to create YCbCr conversion: {}", static_cast<int>(err));
        } else {
            VkSamplerYcbcrConversionInfo ycbcrInfo = {};
            ycbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
            ycbcrInfo.conversion = m_ycbcrConversion;

            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.pNext = &ycbcrInfo;
            viewInfo.image = m_videoImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            err = m_devFuncs->vkCreateImageView(dev, &viewInfo, nullptr, &m_videoImageView);
            if (err != VK_SUCCESS) {
                std::println("Failed to create Vulkan frame image view: {}", static_cast<int>(err));
            } else {
                VkSamplerYcbcrConversionInfo samplerYcbcrInfo = {};
                samplerYcbcrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
                samplerYcbcrInfo.conversion = m_ycbcrConversion;

                VkSamplerCreateInfo samplerInfo = {};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.pNext = &samplerYcbcrInfo;
                samplerInfo.magFilter = VK_FILTER_LINEAR;
                samplerInfo.minFilter = VK_FILTER_LINEAR;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.unnormalizedCoordinates = VK_FALSE;

                err = m_devFuncs->vkCreateSampler(dev, &samplerInfo, nullptr, &m_videoSampler);
                if (err != VK_SUCCESS) {
                    std::println("Failed to create Vulkan frame sampler: {}", static_cast<int>(err));
                } else {
                    createImageDescriptorSet();
                }
            }
        }
    }

    if (vkFramesCtx->unlock_frame) {
        vkFramesCtx->unlock_frame(framesCtx, vkFrame);
    }

    std::println("Using Vulkan zero-copy frame {}x{}", m_videoWidth, m_videoHeight);
}

void ScreenRenderer::createImageDescriptorSet() {
    VkDevice dev = m_window->device();
    VkResult err;
    
    if (!m_imageDescPool) {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 1;
        
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        
        err = m_devFuncs->vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_imageDescPool);
        if (err != VK_SUCCESS) {
            std::println("Failed to create image descriptor pool: {}", static_cast<int>(err));
            return;
        }
    }
    
    if (m_imageDescSetLayout) {
        m_devFuncs->vkDestroyDescriptorSetLayout(dev, m_imageDescSetLayout, nullptr);
        m_imageDescSetLayout = VK_NULL_HANDLE;
    }
    
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &m_videoSampler;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    
    err = m_devFuncs->vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_imageDescSetLayout);
    if (err != VK_SUCCESS) {
        std::println("Failed to create image descriptor set layout: {}", static_cast<int>(err));
        return;
    }
    
    if (m_imageDescSet) {
        m_devFuncs->vkFreeDescriptorSets(dev, m_imageDescPool, 1, &m_imageDescSet);
        m_imageDescSet = VK_NULL_HANDLE;
    }
    
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_imageDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_imageDescSetLayout;
    
    err = m_devFuncs->vkAllocateDescriptorSets(dev, &allocInfo, &m_imageDescSet);
    if (err != VK_SUCCESS) {
        std::println("Failed to allocate image descriptor set: {}", static_cast<int>(err));
        return;
    }
    
    m_imageDescInfo.sampler = VK_NULL_HANDLE;
    m_imageDescInfo.imageView = m_videoImageView;
    m_imageDescInfo.imageLayout = m_videoImageLayout;
    
    VkWriteDescriptorSet writeDesc = {};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.dstSet = m_imageDescSet;
    writeDesc.dstBinding = 0;
    writeDesc.dstArrayElement = 0;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writeDesc.pImageInfo = &m_imageDescInfo;
    
    m_devFuncs->vkUpdateDescriptorSets(dev, 1, &writeDesc, 0, nullptr);
    
    if (m_pipelineLayout) {
        m_devFuncs->vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    }
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_imageDescSetLayout;
    
    err = m_devFuncs->vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (err != VK_SUCCESS) {
        std::println("Failed to recreate pipeline layout: {}", static_cast<int>(err));
        return;
    }
    
    if (m_pipeline) {
        m_devFuncs->vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    
    VkShaderModule vertShaderModule = createShader(QStringLiteral(":/shaders/screen_vert.spv"));
    VkShaderModule fragShaderModule = createShader(QStringLiteral(":/shaders/screen_frag.spv"));
    
    VkGraphicsPipelineCreateInfo pipelineInfo;
    memset(&pipelineInfo, 0, sizeof(pipelineInfo));
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    
    VkPipelineShaderStageCreateInfo shaderStages[2] = {
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,
            VK_SHADER_STAGE_VERTEX_BIT,
            vertShaderModule,
            "main",
            nullptr
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            fragShaderModule,
            "main",
            nullptr
        }
    };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    
    // Vertex input: position (vec2) + UV (vec2) = 4 floats
    VkVertexInputBindingDescription vertexBindingDesc = {
        0, // binding
        4 * sizeof(float), // stride
        VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertexAttrDesc[] = {
        { // position
            0, // location
            0, // binding
            VK_FORMAT_R32G32_SFLOAT,
            0 // offset
        },
        { // UV
            1, // location
            0, // binding
            VK_FORMAT_R32G32_SFLOAT,
            2 * sizeof(float) // offset
        }
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo;
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.pNext = nullptr;
    vertexInputInfo.flags = 0;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &vertexBindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = vertexAttrDesc;
    
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    
    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    pipelineInfo.pInputAssemblyState = &ia;
    
    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    pipelineInfo.pViewportState = &vp;
    
    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    pipelineInfo.pRasterizationState = &rs;
    
    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    pipelineInfo.pMultisampleState = &ms;
    
    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineInfo.pDepthStencilState = &ds;
    
    VkPipelineColorBlendStateCreateInfo cb;
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState att;
    memset(&att, 0, sizeof(att));
    att.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    pipelineInfo.pColorBlendState = &cb;
    
    VkDynamicState dynEnable[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = sizeof(dynEnable) / sizeof(VkDynamicState);
    dyn.pDynamicStates = dynEnable;
    pipelineInfo.pDynamicState = &dyn;
    
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_window->defaultRenderPass();
    
    err = m_devFuncs->vkCreateGraphicsPipelines(dev, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (err != VK_SUCCESS) {
        std::println(stderr, "Failed to create graphics pipeline: {}", static_cast<int>(err));
        if (vertShaderModule)
            m_devFuncs->vkDestroyShaderModule(dev, vertShaderModule, nullptr);
        if (fragShaderModule)
            m_devFuncs->vkDestroyShaderModule(dev, fragShaderModule, nullptr);
        return;
    }
    
    if (vertShaderModule)
        m_devFuncs->vkDestroyShaderModule(dev, vertShaderModule, nullptr);
    if (fragShaderModule)
        m_devFuncs->vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    
    std::println("Created image descriptor set and graphics pipeline");
}


bool ScreenRenderer::ensureCudaLoader() {
    if (m_cu) {
        return true;
    }

    CudaFunctions *loaded = nullptr;
    const int res = cuda_load_functions(&loaded, nullptr);
    if (res != 0 || !loaded) {
        std::println("cuda_load_functions failed: {}", res);
        return false;
    }

    m_cu = loaded;
    const CUresult initRes = m_cu->cuInit(0);
    if (initRes != CUDA_SUCCESS) {
        std::println("cuInit failed: {}", static_cast<int>(initRes));
        return false;
    }

    return true;
}

bool ScreenRenderer::ensureCudaInterop(AVFrame *frame, VkDevice dev, CUcontext cuCtx) {
    const VkDeviceSize yPlaneSize = static_cast<VkDeviceSize>(frame->width) * frame->height;
    const VkDeviceSize uvPlaneSize = yPlaneSize / 2;
    const VkDeviceSize requiredSize = yPlaneSize + uvPlaneSize;

    if (m_cudaStagingBuffer != VK_NULL_HANDLE && m_cudaExternalMemory != nullptr && requiredSize <= m_cudaStagingSize) {
        return true;
    }

    cleanupCudaInterop();

    if (!ensureCudaLoader())
        return false;

    VkExternalMemoryBufferCreateInfo extInfo = {};
    extInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
#ifdef _WIN32
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.pNext = &extInfo;
    bufferInfo.size = requiredSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = m_devFuncs->vkCreateBuffer(dev, &bufferInfo, nullptr, &m_cudaStagingBuffer);
    if (err != VK_SUCCESS) {
        std::println("vkCreateBuffer (CUDA staging) failed: {}", static_cast<int>(err));
        return false;
    }

    VkMemoryRequirements memReqs;
    m_devFuncs->vkGetBufferMemoryRequirements(dev, m_cudaStagingBuffer, &memReqs);

    VkExportMemoryAllocateInfo exportInfo = {};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
#ifdef _WIN32
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = memReqs.size;

    VkPhysicalDeviceMemoryProperties memProps;
    m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(
        m_window->physicalDevice(), &memProps);

    bool memTypeFound = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            memTypeFound = true;
            break;
        }
    }

    if (!memTypeFound) {
        std::println("No suitable memory type for CUDA staging buffer");
        cleanupCudaInterop();
        return false;
    }

    err = m_devFuncs->vkAllocateMemory(dev, &allocInfo, nullptr, &m_cudaStagingMemory);
    if (err != VK_SUCCESS) {
        std::println("vkAllocateMemory (CUDA staging) failed: {}", static_cast<int>(err));
        cleanupCudaInterop();
        return false;
    }

    err = m_devFuncs->vkBindBufferMemory(dev, m_cudaStagingBuffer, m_cudaStagingMemory, 0);
    if (err != VK_SUCCESS) {
        std::println("vkBindBufferMemory (CUDA staging) failed: {}", static_cast<int>(err));
        cleanupCudaInterop();
        return false;
    }

    m_cudaStagingSize = requiredSize;

#ifdef _WIN32
    if (!m_vkGetMemoryWin32HandleKHR) {
        PFN_vkVoidFunction fp = m_window->vulkanInstance()->getInstanceProcAddr("vkGetMemoryWin32HandleKHR");
        m_vkGetMemoryWin32HandleKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(fp);
    }

    if (!m_vkGetMemoryWin32HandleKHR) {
        QVulkanFunctions *instFuncs = m_window->vulkanInstance()->functions();
        if (instFuncs) {
            m_vkGetMemoryWin32HandleKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
                instFuncs->vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR"));
        }
    }

    if (!m_vkGetMemoryWin32HandleKHR) {
        std::println("vkGetMemoryWin32HandleKHR not loaded via vkGetDeviceProcAddr");
        return false;
    }

    VkMemoryGetWin32HandleInfoKHR handleInfo = {};
    handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory = m_cudaStagingMemory;
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    HANDLE winHandle = nullptr;
    err = m_vkGetMemoryWin32HandleKHR(dev, &handleInfo, &winHandle);
    if (err != VK_SUCCESS || !winHandle) {
        std::println("vkGetMemoryWin32HandleKHR failed: {}", static_cast<int>(err));
        cleanupCudaInterop();
        return false;
    }

    m_cu->cuCtxPushCurrent(cuCtx);

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc = {};
    memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
    memDesc.handle.win32.handle = winHandle;
    memDesc.size = m_cudaStagingSize;

    CUresult cuRes = m_cu->cuImportExternalMemory(&m_cudaExternalMemory, &memDesc);
    CloseHandle(winHandle);
#else
    if (!m_vkGetMemoryFdKHR) {
        PFN_vkVoidFunction fp = m_window->vulkanInstance()->getInstanceProcAddr("vkGetMemoryFdKHR");
        m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(fp);
    }

    if (!m_vkGetMemoryFdKHR) {
        QVulkanFunctions *instFuncs = m_window->vulkanInstance()->functions();
        if (instFuncs) {
            m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
                instFuncs->vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
        }
    }

    if (!m_vkGetMemoryFdKHR) {
        std::println("vkGetMemoryFdKHR not loaded via vkGetDeviceProcAddr");
        return false;
    }

    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = m_cudaStagingMemory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    err = m_vkGetMemoryFdKHR(dev, &fdInfo, &fd);
    if (err != VK_SUCCESS || fd < 0) {
        std::println("vkGetMemoryFdKHR failed: {}", static_cast<int>(err));
        cleanupCudaInterop();
        return false;
    }

    m_cu->cuCtxPushCurrent(cuCtx);

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc = {};
    memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memDesc.handle.fd = fd;
    memDesc.size = m_cudaStagingSize;

    CUresult cuRes = m_cu->cuImportExternalMemory(&m_cudaExternalMemory, &memDesc);
    close(fd);
#endif
    if (cuRes != CUDA_SUCCESS) {
        std::println("cuImportExternalMemory failed: {}", static_cast<int>(cuRes));
        m_cu->cuCtxPopCurrent(nullptr);
        cleanupCudaInterop();
        return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {};
    bufferDesc.offset = 0;
    bufferDesc.size = m_cudaStagingSize;
    bufferDesc.flags = 0;

    cuRes = m_cu->cuExternalMemoryGetMappedBuffer(&m_cudaStagingPtr, m_cudaExternalMemory, &bufferDesc);
    m_cu->cuCtxPopCurrent(nullptr);

    if (cuRes != CUDA_SUCCESS) {
        std::println("cuExternalMemoryGetMappedBuffer failed: {}", static_cast<int>(cuRes));
        cleanupCudaInterop();
        return false;
    }

    return true;
}

void ScreenRenderer::cleanupCudaInterop() {
    VkDevice dev = m_window ? m_window->device() : VK_NULL_HANDLE;

    if (m_cu && m_cudaStream) {
        m_cu->cuStreamDestroy(m_cudaStream);
        m_cudaStream = nullptr;
    }

    if (m_cu && m_cudaExternalMemory) {
        m_cu->cuDestroyExternalMemory(m_cudaExternalMemory);
        m_cudaExternalMemory = nullptr;
        m_cudaStagingPtr = 0;
    }

    if (dev != VK_NULL_HANDLE && m_devFuncs) {
        if (m_cudaStagingBuffer)
            m_devFuncs->vkDestroyBuffer(dev, m_cudaStagingBuffer, nullptr);
        if (m_cudaStagingMemory)
            m_devFuncs->vkFreeMemory(dev, m_cudaStagingMemory, nullptr);
    }

    m_cudaStagingBuffer = VK_NULL_HANDLE;
    m_cudaStagingMemory = VK_NULL_HANDLE;
    m_cudaStagingSize = 0;
}

void ScreenRenderer::requestFrameUpdate() {
    if (!m_window) {
        return;
    }

    if (QThread::currentThread() == m_window->thread()) {
        m_window->requestUpdate();
        return;
    }

    QMetaObject::invokeMethod(m_window, [this]() {
        if (m_window) {
            m_window->requestUpdate();
        }
    }, Qt::QueuedConnection);
}

void ScreenRenderer::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkDevice dev = m_window->device();

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_window->graphicsCommandPool();
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    m_devFuncs->vkAllocateCommandBuffers(dev, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_devFuncs->vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = oldLayout;
    barriers[0].newLayout = newLayout;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = oldLayout;
    barriers[1].newLayout = newLayout;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barriers[0].srcAccessMask = 0;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barriers[0].srcAccessMask = 0;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        std::println("Unsupported layout transition");
        return;
    }
    
    m_devFuncs->vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        2, barriers
    );
    
    m_devFuncs->vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    m_devFuncs->vkQueueSubmit(m_window->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    m_devFuncs->vkQueueWaitIdle(m_window->graphicsQueue());
    
    m_devFuncs->vkFreeCommandBuffers(dev, m_window->graphicsCommandPool(), 1, &commandBuffer);
    
    std::println("Image layout transitioned from {} to {}", static_cast<int>(oldLayout), static_cast<int>(newLayout));
}