#include "renderer.hpp"

static float vertexData[] = {
    //  x,     y,    u,   v
    -1.0f, -1.0f,  0.0f, 0.0f, // bottom-left
     1.0f, -1.0f,  1.0f, 0.0f, // bottom-right
    -1.0f,  1.0f,  0.0f, 1.0f, // top-left
     1.0f,  1.0f,  1.0f, 1.0f  // top-right
};

static inline VkDeviceSize aligned(VkDeviceSize v, VkDeviceSize byteAlign) {
    return (v + byteAlign - 1) & ~(byteAlign - 1);
}

Renderer::Renderer(QVulkanWindow *w) : m_window(w) {
    pDecoder = std::make_unique<Decoder>();
    m_decoderThread = std::jthread([this]{
        pDecoder->init();
    });
}

void Renderer::initResources() {
    qDebug("initResources");

    m_device = m_window->device();
    m_devFuncs = m_window->vulkanInstance()->deviceFunctions(m_device);

    const int concurrentFrameCount = m_window->concurrentFrameCount();
    const VkPhysicalDeviceLimits *pdevLimits = &m_window->physicalDeviceProperties()->limits;
    const VkDeviceSize uniAlign = pdevLimits->minUniformBufferOffsetAlignment;
    qDebug("uniform buffer offset alignment is %u", (uint) uniAlign);
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    const VkDeviceSize vertexAllocSize = aligned(sizeof(vertexData), uniAlign);
    const VkDeviceSize uniformAllocSize = aligned(16 * sizeof(float), uniAlign);
    bufInfo.size = vertexAllocSize + concurrentFrameCount * uniformAllocSize;
    bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VkResult err = m_devFuncs->vkCreateBuffer(m_device, &bufInfo, nullptr, &m_buf);
    if (err != VK_SUCCESS) {
        qFatal("Failed to create buffer: %d", err);
    }

    VkMemoryRequirements memReq = {};
    m_devFuncs->vkGetBufferMemoryRequirements(m_device, m_buf, &memReq);

    VkMemoryAllocateInfo memAllocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        memReq.size,
        m_window->hostVisibleMemoryIndex()
    };

    err = m_devFuncs->vkAllocateMemory(m_device, &memAllocInfo, nullptr, &m_bufMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate memory: %d", err);

    err = m_devFuncs->vkBindBufferMemory(m_device, m_buf, m_bufMem, 0);
    if (err != VK_SUCCESS)
        qFatal("Failed to bind buffer memory: %d", err);


    quint8 *p;
    err = m_devFuncs->vkMapMemory(m_device, m_bufMem, 0, memReq.size, 0, reinterpret_cast<void **>(&p));
    if (err != VK_SUCCESS)
        qFatal("Failed to map memory: %d", err);
    memcpy(p, vertexData, sizeof(vertexData));
    QMatrix4x4 ident;
    VkDescriptorBufferInfo m_uniformBufInfo[QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT] = {};
    for (int i = 0; i < concurrentFrameCount; ++i) {
        const VkDeviceSize offset = vertexAllocSize + i * uniformAllocSize;
        memcpy(p + offset, ident.constData(), 16 * sizeof(float));
        m_uniformBufInfo[i].buffer = m_buf;
        m_uniformBufInfo[i].offset = offset;
        m_uniformBufInfo[i].range = uniformAllocSize;
    }
    m_devFuncs->vkUnmapMemory(m_device, m_bufMem);
}

void Renderer::initSwapChainResources() {
    qDebug("initSwapChainResources");
}

void Renderer::releaseSwapChainResources() {
    qDebug("releaseSwapChainResources");
}

void Renderer::releaseResources() {
    qDebug("releaseResources");

    if (m_pipeline) {
        m_devFuncs->vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    if (m_pipelineLayout) {
        m_devFuncs->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_pipelineCache) {
        m_devFuncs->vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }

    if (m_descSetLayout) {
        m_devFuncs->vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);
        m_descSetLayout = VK_NULL_HANDLE;
    }

    if (m_descPool) {
        m_devFuncs->vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }

    if (m_ycbcrSampler) {
        m_devFuncs->vkDestroySampler(m_device, m_ycbcrSampler, nullptr);
        m_ycbcrSampler = VK_NULL_HANDLE;
    }

    if (m_ycbcrConversion) {
        m_devFuncs->vkDestroySamplerYcbcrConversion(m_device, m_ycbcrConversion, nullptr);
        m_ycbcrConversion = VK_NULL_HANDLE;
    }

    if (m_buf) {
        m_devFuncs->vkDestroyBuffer(m_device, m_buf, nullptr);
        m_buf = VK_NULL_HANDLE;
    }

    if (m_bufMem) {
        m_devFuncs->vkFreeMemory(m_device, m_bufMem, nullptr);
        m_bufMem = VK_NULL_HANDLE;
    }
}

void Renderer::startNextFrame() {
    std::println("startNextFrame");
    static std::shared_ptr<AVFrame> lastFrame = nullptr;
    std::shared_ptr<AVFrame> frame = pDecoder->getNextFrame();
    if (frame) {
        lastFrame = frame;
        std::println("Renderer: Pulled new frame from decoder, pts={} format={}", frame->pts, frame->format);

        AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
        AVVulkanFramesContext *vkFramesCtx = static_cast<AVVulkanFramesContext *>(framesCtx->hwctx);
        AVVkFrame *vkFrame = reinterpret_cast<AVVkFrame *>(frame->data[0]);

        if (!vkFrame || !vkFrame->img[0]) {
            std::println("Invalid Vulkan frame data");
            return;
        }

        m_videoImage = vkFrame->img[0];
        m_videoImageLayout = static_cast<VkImageLayout>(vkFrame->layout[0]);
        // createSampler();
        // createPipeline();


        if (!m_ycbcrSampler) {
            createSampler();
        }

        createImageView();

        createImageDescriptorSet();


        if (!m_pipeline) {
            createPipeline();
        }

    } else {
        std::println("Renderer: No new frame available");
    }

    VkCommandBuffer cb = m_window->currentCommandBuffer();
    const QSize sz = m_window->swapChainImageSize();

    VkClearColorValue clearColor = {{ 0, 0, 0, 1 }};
    VkClearDepthStencilValue clearDS = { 1, 0 };
    VkClearValue clearValues[3] = {};
    clearValues[0].color = clearValues[2].color = clearColor;
    clearValues[1].depthStencil = clearDS;

    VkRenderPassBeginInfo rpBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m_window->defaultRenderPass(),
        .framebuffer = m_window->currentFramebuffer(),
        .renderArea = { .extent = {.width = static_cast<uint32_t>(sz.width()), .height = static_cast<uint32_t>(sz.height())}},
        .clearValueCount = static_cast<uint32_t>(m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2),
        .pClearValues = clearValues,
    };

    VkCommandBuffer cmdBuf = m_window->currentCommandBuffer();
    m_devFuncs->vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (m_pipeline) {
        m_devFuncs->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        m_devFuncs->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_imageDescSet, 0, nullptr);
    }

    VkDeviceSize vbOffset = 0;
    m_devFuncs->vkCmdBindVertexBuffers(cb, 0, 1, &m_buf, &vbOffset);

    VkViewport viewport;
    viewport.x = viewport.y = 0;
    viewport.width = sz.width();
    viewport.height = sz.height();
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    m_devFuncs->vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor;
    scissor.offset.x = scissor.offset.y = 0;
    scissor.extent.width = viewport.width;
    scissor.extent.height = viewport.height;
    m_devFuncs->vkCmdSetScissor(cb, 0, 1, &scissor);

    m_devFuncs->vkCmdDraw(cb, 4, 1, 0, 0);

    m_devFuncs->vkCmdEndRenderPass(cmdBuf);

    m_window->frameReady();
    m_window->requestUpdate(); 
}

VkShaderModule Renderer::createShader(const QString &name) {
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("Failed to read shader %s", qPrintable(name));
        return VK_NULL_HANDLE;
    }
    QByteArray blob = file.readAll();
    file.close();

    VkShaderModuleCreateInfo shaderInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(blob.size()),
        .pCode = reinterpret_cast<const uint32_t *>(blob.constData())
    };
 
    VkShaderModule shaderModule;
    VkResult err = m_devFuncs->vkCreateShaderModule(m_window->device(), &shaderInfo, nullptr, &shaderModule);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create shader module: %d", err);
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

void Renderer::createSampler() {
    std::println("createSampler");
    VkResult err;

    VkSamplerYcbcrConversionCreateInfo ycbcrCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .chromaFilter = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
    };

    ycbcrInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .pNext = nullptr,
    };

    VkResult ycbcrErr = m_devFuncs->vkCreateSamplerYcbcrConversion(m_device, &ycbcrCreateInfo, nullptr, &ycbcrInfo.conversion);
    if (ycbcrErr != VK_SUCCESS) {
        qFatal("Failed to create Ycbcr conversion: %d", ycbcrErr);
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &ycbcrInfo,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .unnormalizedCoordinates = VK_FALSE,
    };
 
    err = m_devFuncs->vkCreateSampler(m_device, &samplerInfo, nullptr, &m_ycbcrSampler);
    if (err != VK_SUCCESS) {
        qFatal("Failed to create Ycbcr sampler: %d", err);
    }
}

void Renderer::createImageView() {
    VkResult err;

    if (m_imageView) {
        m_devFuncs->vkDestroyImageView(m_device, m_imageView, nullptr);
    }

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &ycbcrInfo,
        .image = m_videoImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1, 
        },
    };

    err = m_devFuncs->vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView);
    if (err != VK_SUCCESS) {
        std::println("Failed to create image view: {}", static_cast<int>(err));
        return;
    }
}

void Renderer::createPipeline() {
    std::println("createPipeline");
    VkResult err;

    // Pipeline cache
    VkPipelineCacheCreateInfo pipelineCacheInfo = {};
    pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    err = m_devFuncs->vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache);
    if (err != VK_SUCCESS)
        qFatal("Failed to create pipeline cache: %d", err);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descSetLayout;
    err = m_devFuncs->vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (err != VK_SUCCESS)
        qFatal("Failed to create pipeline layout: %d", err);

    // Shaders
    VkShaderModule vertShaderModule = createShader(QStringLiteral(":/shaders/screen_vert.spv"));
    VkShaderModule fragShaderModule = createShader(QStringLiteral(":/shaders/screen_frag.spv"));

    // Graphics pipeline
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

    VkVertexInputBindingDescription vertexBindingDesc = {
        .binding = 0,
        .stride = 4 * sizeof(float),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription vertexAttrDesc[] = {
        { // position
            .location = 0,
            .binding = 0,
            .format= VK_FORMAT_R32G32_SFLOAT,
            .offset = 0
        },
        { // color
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = 2 * sizeof(float)
        }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBindingDesc,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = vertexAttrDesc
    };

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
    };

    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = m_window->sampleCountFlagBits()
    };

    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL
    };

    VkPipelineColorBlendAttachmentState att = {
        .colorWriteMask = 0xF
    };

    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att
    };

    VkDynamicState dynEnable[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dynEnable) / sizeof(VkDynamicState),
        .pDynamicStates = dynEnable
    };

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = m_pipelineLayout,
        .renderPass = m_window->defaultRenderPass()
    };

    err = m_devFuncs->vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (err != VK_SUCCESS)
        qFatal("Failed to create graphics pipeline: %d", err);

    if (vertShaderModule) {
        m_devFuncs->vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    }
    if (fragShaderModule) {
        m_devFuncs->vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    }
}

void Renderer::createImageDescriptorSet() {
    std::println("createImageDescriptorSet");
    VkResult err;
    const int concurrentFrameCount = m_window->concurrentFrameCount();

    if (m_descPool) {
        m_devFuncs->vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    }

    if (m_descSetLayout) {
        m_devFuncs->vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);
    }

    VkDescriptorPoolSize descPoolSizes = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(concurrentFrameCount) };
    VkDescriptorPoolCreateInfo descPoolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = static_cast<uint32_t>(concurrentFrameCount),
        .poolSizeCount = 1,
        .pPoolSizes = &descPoolSizes
    };
    err = m_devFuncs->vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &m_descPool);
    if (err != VK_SUCCESS) {
        qFatal("Failed to create descriptor pool: %d", err);
    }

    VkDescriptorSetLayoutBinding image_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &m_ycbcrSampler
    };

    VkDescriptorSetLayoutCreateInfo descLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &image_binding
    };

    err = m_devFuncs->vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_descSetLayout);
    if (err != VK_SUCCESS)
        qFatal("Failed to create descriptor set layout: %d", err);

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &m_descSetLayout
    };
    err = m_devFuncs->vkAllocateDescriptorSets(m_device, &allocInfo, &m_imageDescSet);
    if (err != VK_SUCCESS) {
        std::println("Failed to allocate image descriptor set: {}", static_cast<int>(err));
        return;
    }
    
    VkDescriptorImageInfo m_imageDescInfo = {
        .sampler = m_ycbcrSampler,
        .imageView = m_imageView,
        .imageLayout = m_videoImageLayout
    };
    
    VkWriteDescriptorSet writeDesc = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = m_imageDescSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &m_imageDescInfo
    };
    
    m_devFuncs->vkUpdateDescriptorSets(m_device, 1, &writeDesc, 0, nullptr);
}
