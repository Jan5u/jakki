#include "nvenc_linux.hpp"
#include "../../network.hpp"

NvencLinuxEncoder::NvencLinuxEncoder(Network* network, EncoderType type) : m_network(network), m_encoder_type(type) {}

NvencLinuxEncoder::~NvencLinuxEncoder() {
    flush();
    
    if (codec_ctx) {
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, packet) >= 0) {
            if (output_file) {
                fwrite(packet->data, 1, packet->size, output_file);
            }
            av_packet_unref(packet);
        }
        avcodec_free_context(&codec_ctx);
    }
    if (packet) av_packet_free(&packet);
    if (hw_frames_ctx) av_buffer_unref(&hw_frames_ctx);
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    if (output_file) fclose(output_file);
    if (bgra_file) fclose(bgra_file);
    if (nv12_file) fclose(nv12_file);
    
    if (cu && cuda_ctx) {
        cu->cuCtxPushCurrent(cuda_ctx);
        if (cuda_stream) cu->cuStreamDestroy(cuda_stream);
        cu->cuCtxPopCurrent(nullptr);
    }
    
    if (vk_device != VK_NULL_HANDLE && vk_dev_funcs) {
        cleanupFrameResources();
        
        if (vk_compute_pipeline != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyPipeline(vk_device, vk_compute_pipeline, nullptr);
        }
        if (vk_pipeline_layout != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyPipelineLayout(vk_device, vk_pipeline_layout, nullptr);
        }
        if (vk_descriptor_pool != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyDescriptorPool(vk_device, vk_descriptor_pool, nullptr);
        }
        if (vk_descriptor_set_layout != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyDescriptorSetLayout(vk_device, vk_descriptor_set_layout, nullptr);
        }
        if (vk_shader_module != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyShaderModule(vk_device, vk_shader_module, nullptr);
        }
        if (vk_command_pool != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroyCommandPool(vk_device, vk_command_pool, nullptr);
        }
        if (vk_timeline_semaphore != VK_NULL_HANDLE) {
            vk_dev_funcs->vkDestroySemaphore(vk_device, vk_timeline_semaphore, nullptr);
        }
        
        vk_dev_funcs->vkDestroyDevice(vk_device, nullptr);
        vk_device = VK_NULL_HANDLE;
    }
    
    if (vk_instance) {
        vk_instance->destroy();
        delete vk_instance;
        vk_instance = nullptr;
    }
}

bool NvencLinuxEncoder::isReady() const {
    return m_ready;
}

std::string NvencLinuxEncoder::getName() const {
    return Encoder::encoderTypeToName(m_encoder_type);
}

const char* NvencLinuxEncoder::getFFmpegEncoderName() const {
    switch (m_encoder_type) {
        case EncoderType::NVENC_H264: return "h264_nvenc";
        case EncoderType::NVENC_HEVC: return "hevc_nvenc";
        case EncoderType::NVENC_AV1:  return "av1_nvenc";
        default: return "h264_nvenc";
    }
}

void NvencLinuxEncoder::init() {
    vk_instance = new QVulkanInstance();
    vk_instance->setApiVersion(QVersionNumber(1, 3));
    vk_instance->setLayers({"VK_LAYER_KHRONOS_validation"});

    if (!vk_instance->create()) {
        std::println("Failed to create Vulkan instance");
        delete vk_instance;
        vk_instance = nullptr;
        return;
    }
    
    std::println("Vulkan instance created successfully");
    
    vk_funcs = vk_instance->functions();
    if (!vk_funcs) {
        std::println("Failed to get Vulkan functions");
        return;
    }
    
    uint32_t deviceCount = 0;
    vk_funcs->vkEnumeratePhysicalDevices(vk_instance->vkInstance(), &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::println("Failed to find GPUs with Vulkan support");
        return;
    }
    
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vk_funcs->vkEnumeratePhysicalDevices(vk_instance->vkInstance(), &deviceCount, devices.data());
    vk_physical_device = devices[0];
    
    uint32_t queueFamilyCount = 0;
    vk_funcs->vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vk_funcs->vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queueFamilyCount, queueFamilies.data());
    
    vk_queue_family_index = 0;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            vk_queue_family_index = i;
            break;
        }
    }
    
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = vk_queue_family_index;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    
    const char* deviceExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
    };
    
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures{};
    timelineSemaphoreFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineSemaphoreFeatures.timelineSemaphore = VK_TRUE;
    
    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &timelineSemaphoreFeatures;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 4;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
    
    if (vk_funcs->vkCreateDevice(vk_physical_device, &deviceCreateInfo, nullptr, &vk_device) != VK_SUCCESS) {
        std::println("Failed to create Vulkan device");
        return;
    }
    
    vk_dev_funcs = vk_instance->deviceFunctions(vk_device);
    if (!vk_dev_funcs) {
        std::println("Failed to get Vulkan device functions");
        return;
    }
    
    std::println("Vulkan device created");
    
    vk_dev_funcs->vkGetDeviceQueue(vk_device, vk_queue_family_index, 0, &vk_compute_queue);
    
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = vk_queue_family_index;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    
    if (vk_dev_funcs->vkCreateCommandPool(vk_device, &poolInfo, nullptr, &vk_command_pool) != VK_SUCCESS) {
        std::println("Failed to create command pool");
        return;
    }
    
    VkDescriptorSetLayoutBinding bindings[3] = {};
    
    // Input BGRA image
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    // Output Y plane
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    // Output UV plane
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    
    if (vk_dev_funcs->vkCreateDescriptorSetLayout(vk_device, &layoutInfo, nullptr, &vk_descriptor_set_layout) != VK_SUCCESS) {
        std::println("Failed to create descriptor set layout");
        return;
    }
    
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 300;
    
    VkDescriptorPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCreateInfo.poolSizeCount = 1;
    poolCreateInfo.pPoolSizes = &poolSize;
    poolCreateInfo.maxSets = 100;
    
    if (vk_dev_funcs->vkCreateDescriptorPool(vk_device, &poolCreateInfo, nullptr, &vk_descriptor_pool) != VK_SUCCESS) {
        std::println("Failed to create descriptor pool");
        return;
    }

    VkShaderModule vk_shader_module = createShader(QStringLiteral(":/shaders/bgra_to_nv12_comp.spv"));
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &vk_descriptor_set_layout;
    
    if (vk_dev_funcs->vkCreatePipelineLayout(vk_device, &pipelineLayoutInfo, nullptr, &vk_pipeline_layout) != VK_SUCCESS) {
        std::println("Failed to create pipeline layout");
        return;
    }
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = vk_shader_module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = vk_pipeline_layout;
    
    if (vk_dev_funcs->vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vk_compute_pipeline) != VK_SUCCESS) {
        std::println("Failed to create compute pipeline");
        return;
    }
    
    VkSemaphoreTypeCreateInfo timelineCreateInfo{};
    timelineCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;
    
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &timelineCreateInfo;
    
    if (vk_dev_funcs->vkCreateSemaphore(vk_device, &semaphoreInfo, nullptr, &vk_timeline_semaphore) != VK_SUCCESS) {
        std::println("Failed to create timeline semaphore");
        return;
    }
    
    if (!initCUDA()) {
        std::println("Failed to initialize CUDA");
        return;
    }
    
    m_ready = true;
    std::println("NvencLinuxEncoder initialized successfully with {}", getName());
}

bool NvencLinuxEncoder::initCUDA() {
    int err = cuda_load_functions(&cu, nullptr);
    if (err < 0 || !cu) {
        std::println("Failed to load CUDA functions: {}", err);
        return false;
    }
    
    CUresult res = cu->cuInit(0);
    if (res != CUDA_SUCCESS) {
        std::println("Failed to initialize CUDA: {}", (int)res);
        return false;
    }
    
    std::println("CUDA initialized successfully (context will be obtained from FFmpeg)");
    return true;
}

bool NvencLinuxEncoder::initFFmpegEncoder(int width, int height) {
    const char* encoder_name = getFFmpegEncoderName();
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        std::println("{} encoder not found", encoder_name);
        return false;
    }
    
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::println("Failed to allocate codec context");
        return false;
    }
    
    int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0) {
        std::println("Failed to create CUDA device context");
        return false;
    }
    
    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ctx) {
        std::println("Failed to allocate hardware frames context");
        return false;
    }
    
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = width;
    frames_ctx->height = height;
    frames_ctx->initial_pool_size = 0;
    
    ret = av_hwframe_ctx_init(hw_frames_ctx);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::println("Failed to initialize hardware frames context: {}", errbuf);
        return false;
    }
    
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = {1, 60};
    codec_ctx->framerate = {60, 1};
    codec_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    codec_ctx->gop_size = 60;
    
    av_opt_set(codec_ctx->priv_data, "preset", "p4", 0);
    av_opt_set(codec_ctx->priv_data, "tune", "ll", 0);
    av_opt_set(codec_ctx->priv_data, "rc", "vbr", 0);
    av_opt_set_int(codec_ctx->priv_data, "b", 8000000, 0);
    av_opt_set_int(codec_ctx->priv_data, "maxrate", 10000000, 0);
    av_opt_set_int(codec_ctx->priv_data, "bufsize", 16000000, 0);
    av_opt_set_int(codec_ctx->priv_data, "forced-idr", 1, 0);
    av_opt_set_int(codec_ctx->priv_data, "repeat-headers", 1, 0);
    av_opt_set_int(codec_ctx->priv_data, "delay", 0, 0);
    
    av_log_set_level(AV_LOG_DEBUG);

    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::println("Failed to open encoder: {}", errbuf);
        return false;
    }
    
    packet = av_packet_alloc();
    if (!packet) {
        std::println("Failed to allocate packet");
        return false;
    }
    
    AVHWDeviceContext *hw_dev_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    AVCUDADeviceContext *cuda_dev_ctx = (AVCUDADeviceContext*)hw_dev_ctx->hwctx;
    cuda_ctx = cuda_dev_ctx->cuda_ctx;
    
    cu->cuCtxPushCurrent(cuda_ctx);
    CUresult cuda_res = cu->cuStreamCreate(&cuda_stream, CU_STREAM_NON_BLOCKING);
    cu->cuCtxPopCurrent(nullptr);
    
    if (cuda_res != CUDA_SUCCESS) {
        std::println("Failed to create CUDA stream: {}", (int)cuda_res);
        return false;
    }

    std::println("{} encoder initialized: {}x{}, using FFmpeg CUDA context", encoder_name, width, height);
    return true;
}

void NvencLinuxEncoder::initFrameResources() {
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = vk_command_pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    
    std::vector<VkCommandBuffer> commandBuffers(MAX_FRAMES_IN_FLIGHT);
    vk_dev_funcs->vkAllocateCommandBuffers(vk_device, &cmdAllocInfo, commandBuffers.data());
    
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, vk_descriptor_set_layout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vk_descriptor_pool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();
    
    std::vector<VkDescriptorSet> descriptorSets(MAX_FRAMES_IN_FLIGHT);
    vk_dev_funcs->vkAllocateDescriptorSets(vk_device, &allocInfo, descriptorSets.data());
    
    frame_resources.resize(MAX_FRAMES_IN_FLIGHT);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        frame_resources[i].command_buffer = commandBuffers[i];
        frame_resources[i].descriptor_set = descriptorSets[i];
        frame_resources[i].y_image = VK_NULL_HANDLE;
        frame_resources[i].uv_image = VK_NULL_HANDLE;
        frame_resources[i].y_view = VK_NULL_HANDLE;
        frame_resources[i].uv_view = VK_NULL_HANDLE;
        frame_resources[i].y_memory = VK_NULL_HANDLE;
        frame_resources[i].uv_memory = VK_NULL_HANDLE;
        frame_resources[i].cuda_ext_mem_y = nullptr;
        frame_resources[i].cuda_ext_mem_uv = nullptr;
        frame_resources[i].cuda_ptr_y = 0;
        frame_resources[i].cuda_ptr_uv = 0;
        frame_resources[i].in_use = false;
    }
    
    std::println("Frame resource pool initialized");
}

void NvencLinuxEncoder::cleanupFrameResources() {
    for (auto& res : frame_resources) {
        if (res.cuda_ext_mem_y && cu) cu->cuDestroyExternalMemory(res.cuda_ext_mem_y);
        if (res.cuda_ext_mem_uv && cu) cu->cuDestroyExternalMemory(res.cuda_ext_mem_uv);
        if (res.y_view != VK_NULL_HANDLE) vk_dev_funcs->vkDestroyImageView(vk_device, res.y_view, nullptr);
        if (res.uv_view != VK_NULL_HANDLE) vk_dev_funcs->vkDestroyImageView(vk_device, res.uv_view, nullptr);
        if (res.y_image != VK_NULL_HANDLE) vk_dev_funcs->vkDestroyImage(vk_device, res.y_image, nullptr);
        if (res.uv_image != VK_NULL_HANDLE) vk_dev_funcs->vkDestroyImage(vk_device, res.uv_image, nullptr);
        if (res.y_memory != VK_NULL_HANDLE) vk_dev_funcs->vkFreeMemory(vk_device, res.y_memory, nullptr);
        if (res.uv_memory != VK_NULL_HANDLE) vk_dev_funcs->vkFreeMemory(vk_device, res.uv_memory, nullptr);
    }
    frame_resources.clear();
}

int NvencLinuxEncoder::acquireFrameResources() {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!frame_resources[i].in_use) {
            frame_resources[i].in_use = true;
            return i;
        }
    }
    return -1;
}

bool NvencLinuxEncoder::encodeDmaBufFrame(int dma_fd, int width, int height, int stride, uint64_t modifier) { 
    if (frame_resources.empty()) {
        initFrameResources();
        if (!initFFmpegEncoder(width, height)) {
            return false;
        }
        current_width = width;
        current_height = height;
    }
    
    if (width != current_width || height != current_height) {
        flush();
        cleanupFrameResources();
        initFrameResources();
        current_width = width;
        current_height = height;
    }
    
    cleanupCompletedFrames();
    
    int frame_idx = acquireFrameResources();
    if (frame_idx < 0) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &vk_timeline_semaphore;
        waitInfo.pValues = &pending_frames.front().timeline_value;
        vk_dev_funcs->vkWaitSemaphores(vk_device, &waitInfo, UINT64_MAX);
        cleanupCompletedFrames();
        frame_idx = acquireFrameResources();
    }
    
    auto& res = frame_resources[frame_idx];
    
    if (res.y_image == VK_NULL_HANDLE) {
        res.y_image = createNV12Image(width, height, VK_IMAGE_ASPECT_PLANE_0_BIT, VK_FORMAT_R8_UNORM, res.y_memory);
        res.uv_image = createNV12Image(width / 2, height / 2, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_FORMAT_R8G8_UNORM, res.uv_memory);
        res.y_view = createImageView(res.y_image, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        res.uv_view = createImageView(res.uv_image, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        
        if (!setupCUDAInterop(res, width, height)) {
            frame_resources[frame_idx].in_use = false;
            return false;
        }
    }
    
    VkDeviceMemory inputMemory = VK_NULL_HANDLE;
    VkImage inputImage = importDmaBufAsImage(dma_fd, width, height, modifier, inputMemory);
    if (inputImage == VK_NULL_HANDLE) {
        frame_resources[frame_idx].in_use = false;
        return false;
    }
    
    VkImageView inputView = createImageView(inputImage, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    
    VkDescriptorImageInfo imageInfos[3] = {};
    imageInfos[0].imageView = inputView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[1].imageView = res.y_view;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[2].imageView = res.uv_view;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    
    VkWriteDescriptorSet descriptorWrites[3] = {};
    for (int i = 0; i < 3; i++) {
        descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[i].dstSet = res.descriptor_set;
        descriptorWrites[i].dstBinding = i;
        descriptorWrites[i].dstArrayElement = 0;
        descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrites[i].descriptorCount = 1;
        descriptorWrites[i].pImageInfo = &imageInfos[i];
    }
    
    vk_dev_funcs->vkUpdateDescriptorSets(vk_device, 3, descriptorWrites, 0, nullptr);
    
    vk_dev_funcs->vkResetCommandBuffer(res.command_buffer, 0);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vk_dev_funcs->vkBeginCommandBuffer(res.command_buffer, &beginInfo);
    
    transitionImageLayout(res.command_buffer, inputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(res.command_buffer, res.y_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transitionImageLayout(res.command_buffer, res.uv_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    
    vk_dev_funcs->vkCmdBindPipeline(res.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_compute_pipeline);
    vk_dev_funcs->vkCmdBindDescriptorSets(res.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline_layout, 0, 1, &res.descriptor_set, 0, nullptr);
    
    uint32_t groupCountX = (width + 15) / 16;
    uint32_t groupCountY = (height + 15) / 16;
    vk_dev_funcs->vkCmdDispatch(res.command_buffer, groupCountX, groupCountY, 1);
    
    vk_dev_funcs->vkEndCommandBuffer(res.command_buffer);
    
    vk_timeline_value++;
    
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &vk_timeline_value;
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = &timelineInfo;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &res.command_buffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &vk_timeline_semaphore;
    
    vk_dev_funcs->vkQueueSubmit(vk_compute_queue, 1, &submitInfo, VK_NULL_HANDLE);
    
    PendingFrame pending;
    pending.timeline_value = vk_timeline_value;
    pending.frame_index = frame_idx;
    pending.input_image = inputImage;
    pending.input_view = inputView;
    pending.input_memory = inputMemory;
    
    pending_frames.push_back(pending);
    
    encodeFrame(res, vk_timeline_value);
    
    return true;
}

bool NvencLinuxEncoder::setupCUDAInterop(FrameResources& res, int width, int height) {
    CUcontext oldCtx;
    cu->cuCtxPushCurrent(cuda_ctx);
    
    VkMemoryRequirements memReqY;
    vk_dev_funcs->vkGetImageMemoryRequirements(vk_device, res.y_image, &memReqY);
    
    VkMemoryRequirements memReqUV;
    vk_dev_funcs->vkGetImageMemoryRequirements(vk_device, res.uv_image, &memReqUV);
    
    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = res.y_memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    
    auto vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vk_funcs->vkGetDeviceProcAddr(vk_device, "vkGetMemoryFdKHR");
    if (!vkGetMemoryFdKHR) {
        std::println("Failed to get vkGetMemoryFdKHR");
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    int fd_y;
    if (vkGetMemoryFdKHR(vk_device, &fdInfo, &fd_y) != VK_SUCCESS) {
        std::println("Failed to get fd for Y plane");
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    fdInfo.memory = res.uv_memory;
    int fd_uv;
    if (vkGetMemoryFdKHR(vk_device, &fdInfo, &fd_uv) != VK_SUCCESS) {
        std::println("Failed to get fd for UV plane");
        close(fd_y);
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc{};
    memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memDesc.handle.fd = fd_y;
    memDesc.size = memReqY.size;
    
    CUresult res_cu = cu->cuImportExternalMemory(&res.cuda_ext_mem_y, &memDesc);
    if (res_cu != CUDA_SUCCESS) {
        std::println("Failed to import Y plane to CUDA: {} (size: {})", (int)res_cu, memDesc.size);
        close(fd_y);
        close(fd_uv);
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    memDesc.handle.fd = fd_uv;
    memDesc.size = memReqUV.size;
    
    res_cu = cu->cuImportExternalMemory(&res.cuda_ext_mem_uv, &memDesc);
    if (res_cu != CUDA_SUCCESS) {
        std::println("Failed to import UV plane to CUDA: {}", (int)res_cu);
        close(fd_uv);
        cu->cuDestroyExternalMemory(res.cuda_ext_mem_y);
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    close(fd_y);
    close(fd_uv);
    
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc{};
    bufDesc.offset = 0;
    bufDesc.size = memReqY.size;
    
    res_cu = cu->cuExternalMemoryGetMappedBuffer(&res.cuda_ptr_y, res.cuda_ext_mem_y, &bufDesc);
    if (res_cu != CUDA_SUCCESS) {
        std::println("Failed to map Y buffer: {}", (int)res_cu);
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    bufDesc.size = memReqUV.size;
    res_cu = cu->cuExternalMemoryGetMappedBuffer(&res.cuda_ptr_uv, res.cuda_ext_mem_uv, &bufDesc);
    if (res_cu != CUDA_SUCCESS) {
        std::println("Failed to map UV buffer: {}", (int)res_cu);
        cu->cuCtxPopCurrent(nullptr);
        return false;
    }
    
    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.arrayLayer = 0;
    
    VkSubresourceLayout layout_y;
    vk_dev_funcs->vkGetImageSubresourceLayout(vk_device, res.y_image, &subresource, &layout_y);
    res.y_pitch = layout_y.rowPitch;
    
    VkSubresourceLayout layout_uv;
    vk_dev_funcs->vkGetImageSubresourceLayout(vk_device, res.uv_image, &subresource, &layout_uv);
    res.uv_pitch = layout_uv.rowPitch;
    
    cu->cuCtxPopCurrent(nullptr);
    
    std::println("CUDA interop setup complete for frame resource (Y pitch: {}, UV pitch: {})", res.y_pitch, res.uv_pitch);
    return true;
}

bool NvencLinuxEncoder::encodeFrame(FrameResources& res, uint64_t timeline_value) {
    uint64_t current_value = 0;
    vk_dev_funcs->vkGetSemaphoreCounterValue(vk_device, vk_timeline_semaphore, &current_value);
    
    if (current_value < timeline_value) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &vk_timeline_semaphore;
        waitInfo.pValues = &timeline_value;
        vk_dev_funcs->vkWaitSemaphores(vk_device, &waitInfo, UINT64_MAX);
    }
    
    int width = codec_ctx->width;
    int height = codec_ctx->height;
    
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        std::println("Failed to allocate AVFrame");
        return false;
    }
    
    int ret = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::println("Failed to get hardware frame buffer: {}", errbuf);
        av_frame_free(&frame);
        return false;
    }
    
    frame->pts = frame_count++;
    
    CUdeviceptr dst_y = (CUdeviceptr)frame->data[0];
    CUdeviceptr dst_uv = (CUdeviceptr)frame->data[1];
    int dst_y_pitch = frame->linesize[0];
    int dst_uv_pitch = frame->linesize[1];

    cu->cuCtxPushCurrent(cuda_ctx);
    
    CUDA_MEMCPY2D copyY = {};
    copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.srcDevice = res.cuda_ptr_y;
    copyY.srcPitch = res.y_pitch;
    copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.dstDevice = dst_y;
    copyY.dstPitch = dst_y_pitch;
    copyY.WidthInBytes = width;
    copyY.Height = height;
    
    CUresult cuda_res = cu->cuMemcpy2D(&copyY);
    if (cuda_res != CUDA_SUCCESS) {
        std::println("Failed to copy Y plane (GPU->GPU): {}", (int)cuda_res);
        cu->cuCtxPopCurrent(nullptr);
        av_frame_free(&frame);
        return false;
    }
    
    CUDA_MEMCPY2D copyUV = {};
    copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.srcDevice = res.cuda_ptr_uv;
    copyUV.srcPitch = res.uv_pitch;
    copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.dstDevice = dst_uv;
    copyUV.dstPitch = dst_uv_pitch;
    copyUV.WidthInBytes = width;
    copyUV.Height = height / 2;
    
    cuda_res = cu->cuMemcpy2D(&copyUV);
    if (cuda_res != CUDA_SUCCESS) {
        std::println("Failed to copy UV plane (GPU->GPU): {}", (int)cuda_res);
        cu->cuCtxPopCurrent(nullptr);
        av_frame_free(&frame);
        return false;
    }
    
    cu->cuCtxPopCurrent(nullptr);
    
    ret = avcodec_send_frame(codec_ctx, frame);
    av_frame_free(&frame);
    
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::println("Failed to send frame to encoder: {}", errbuf);
        return false;
    }
    
    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            std::println("Error receiving packet");
            return false;
        }
        
        if (output_file) {
            fwrite(packet->data, 1, packet->size, output_file);
        }
        
        if (m_network) {
            std::vector<uint8_t> framedPacket;
            uint32_t packetSize = packet->size;
            
            framedPacket.push_back(packetSize & 0xFF);
            framedPacket.push_back((packetSize >> 8) & 0xFF);
            framedPacket.push_back((packetSize >> 16) & 0xFF);
            framedPacket.push_back((packetSize >> 24) & 0xFF);
            
            framedPacket.insert(framedPacket.end(), packet->data, packet->data + packet->size);
            
            m_network->sendScreensharePackets(framedPacket);
            std::println("Sent encoded packet: {} bytes (payload: {})", framedPacket.size(), packet->size);
        }
        
        av_packet_unref(packet);
    }
    
    return true;
}

void NvencLinuxEncoder::cleanupCompletedFrames() {
    if (pending_frames.empty()) return;
    
    uint64_t current_value = 0;
    vk_dev_funcs->vkGetSemaphoreCounterValue(vk_device, vk_timeline_semaphore, &current_value);
    
    while (!pending_frames.empty() && pending_frames.front().timeline_value <= current_value) {
        const auto& frame = pending_frames.front();
        
        vk_dev_funcs->vkDestroyImageView(vk_device, frame.input_view, nullptr);
        vk_dev_funcs->vkDestroyImage(vk_device, frame.input_image, nullptr);
        vk_dev_funcs->vkFreeMemory(vk_device, frame.input_memory, nullptr);
        
        frame_resources[frame.frame_index].in_use = false;
        
        pending_frames.erase(pending_frames.begin());
    }
}

void NvencLinuxEncoder::flush() {
    if (pending_frames.empty()) return;
    
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &vk_timeline_semaphore;
    waitInfo.pValues = &pending_frames.back().timeline_value;
    
    vk_dev_funcs->vkWaitSemaphores(vk_device, &waitInfo, UINT64_MAX);
    
    cleanupCompletedFrames();
}

VkShaderModule NvencLinuxEncoder::createShader(const QString &name) {
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
    VkResult err = vk_dev_funcs->vkCreateShaderModule(vk_device, &shaderInfo, nullptr, &shaderModule);
    if (err != VK_SUCCESS) {
        qWarning("Failed to create shader module: %d", err);
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

VkImage NvencLinuxEncoder::importDmaBufAsImage(int dma_fd, int width, int height, uint64_t modifier, VkDeviceMemory& memory) {
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{};
    VkSubresourceLayout planeLayout{};
    
    bool useModifier = (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID);
    
    if (useModifier) {
        planeLayout.offset = 0;
        planeLayout.size = 0;
        planeLayout.rowPitch = width * 4;
        planeLayout.arrayPitch = 0;
        planeLayout.depthPitch = 0;
        
        modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modifierInfo.drmFormatModifier = modifier;
        modifierInfo.drmFormatModifierPlaneCount = 1;
        modifierInfo.pPlaneLayouts = &planeLayout;
        
    } else {
        std::println("Importing DMA-BUF as linear");
    }
    
    VkExternalMemoryImageCreateInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.pNext = useModifier ? &modifierInfo : nullptr;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = useModifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage image;
    if (vk_dev_funcs->vkCreateImage(vk_device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        std::println("Failed to create image for DMA-BUF import");
        return VK_NULL_HANDLE;
    }
    
    VkMemoryRequirements memRequirements;
    vk_dev_funcs->vkGetImageMemoryRequirements(vk_device, image, &memRequirements);
    
    int dup_fd = dup(dma_fd);
    if (dup_fd < 0) {
        std::println("Failed to duplicate DMA-BUF file descriptor");
        vk_dev_funcs->vkDestroyImage(vk_device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = dup_fd;
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = 0;
    
    VkPhysicalDeviceMemoryProperties memProperties;
    vk_funcs->vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    
    if (vk_dev_funcs->vkAllocateMemory(vk_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        std::println("Failed to allocate memory for DMA-BUF");
        vk_dev_funcs->vkDestroyImage(vk_device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    if (vk_dev_funcs->vkBindImageMemory(vk_device, image, memory, 0) != VK_SUCCESS) {
        std::println("Failed to bind image memory");
        vk_dev_funcs->vkFreeMemory(vk_device, memory, nullptr);
        vk_dev_funcs->vkDestroyImage(vk_device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    return image;
}

VkImage NvencLinuxEncoder::createNV12Image(int width, int height, VkImageAspectFlags aspect, VkFormat format, VkDeviceMemory& memory) {
    VkExternalMemoryImageCreateInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage image;
    if (vk_dev_funcs->vkCreateImage(vk_device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    VkMemoryRequirements memRequirements;
    vk_dev_funcs->vkGetImageMemoryRequirements(vk_device, image, &memRequirements);
    
    VkPhysicalDeviceMemoryProperties memProperties;
    vk_funcs->vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memProperties);
    
    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = image;
    
    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.pNext = &dedicatedInfo;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = memRequirements.size;
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    
    if (vk_dev_funcs->vkAllocateMemory(vk_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vk_dev_funcs->vkDestroyImage(vk_device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    vk_dev_funcs->vkBindImageMemory(vk_device, image, memory, 0);
    return image;
}

VkImageView NvencLinuxEncoder::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    VkImageView imageView;
    if (vk_dev_funcs->vkCreateImageView(vk_device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    return imageView;
}

void NvencLinuxEncoder::transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    
    vk_dev_funcs->vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void NvencLinuxEncoder::saveBGRAFrame(VkImage image, int width, int height) {
    if (!bgra_file) return;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = width * height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer stagingBuffer;
    if (vk_dev_funcs->vkCreateBuffer(vk_device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkMemoryRequirements memRequirements;
    vk_dev_funcs->vkGetBufferMemoryRequirements(vk_device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    
    VkPhysicalDeviceMemoryProperties memProperties;
    vk_funcs->vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    
    VkDeviceMemory stagingMemory;
    if (vk_dev_funcs->vkAllocateMemory(vk_device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vk_dev_funcs->vkDestroyBuffer(vk_device, stagingBuffer, nullptr);
        return;
    }
    
    vk_dev_funcs->vkBindBufferMemory(vk_device, stagingBuffer, stagingMemory, 0);
    
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = vk_command_pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    vk_dev_funcs->vkAllocateCommandBuffers(vk_device, &cmdAllocInfo, &cmdBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vk_dev_funcs->vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    vk_dev_funcs->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
    
    vk_dev_funcs->vkCmdCopyImageToBuffer(cmdBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);
    
    vk_dev_funcs->vkEndCommandBuffer(cmdBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    vk_dev_funcs->vkQueueSubmit(vk_compute_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vk_dev_funcs->vkQueueWaitIdle(vk_compute_queue);
    
    void* data;
    vk_dev_funcs->vkMapMemory(vk_device, stagingMemory, 0, width * height * 4, 0, &data);
    fwrite(data, 1, width * height * 4, bgra_file);
    fflush(bgra_file);
    vk_dev_funcs->vkUnmapMemory(vk_device, stagingMemory);
    
    vk_dev_funcs->vkFreeCommandBuffers(vk_device, vk_command_pool, 1, &cmdBuffer);
    vk_dev_funcs->vkFreeMemory(vk_device, stagingMemory, nullptr);
    vk_dev_funcs->vkDestroyBuffer(vk_device, stagingBuffer, nullptr);
}

void NvencLinuxEncoder::saveNV12Frame(FrameResources& res, int width, int height) {
    if (!nv12_file) return;
    
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &vk_timeline_semaphore;
    waitInfo.pValues = &vk_timeline_value;
    
    vk_dev_funcs->vkWaitSemaphores(vk_device, &waitInfo, UINT64_MAX);
    
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2) * 2;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = y_size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer y_stagingBuffer;
    vk_dev_funcs->vkCreateBuffer(vk_device, &bufferInfo, nullptr, &y_stagingBuffer);
    
    VkMemoryRequirements memReq;
    vk_dev_funcs->vkGetBufferMemoryRequirements(vk_device, y_stagingBuffer, &memReq);
    
    VkPhysicalDeviceMemoryProperties memProps;
    vk_funcs->vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memProps);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    
    VkDeviceMemory y_stagingMemory;
    vk_dev_funcs->vkAllocateMemory(vk_device, &allocInfo, nullptr, &y_stagingMemory);
    vk_dev_funcs->vkBindBufferMemory(vk_device, y_stagingBuffer, y_stagingMemory, 0);
    
    bufferInfo.size = uv_size;
    VkBuffer uv_stagingBuffer;
    vk_dev_funcs->vkCreateBuffer(vk_device, &bufferInfo, nullptr, &uv_stagingBuffer);
    
    vk_dev_funcs->vkGetBufferMemoryRequirements(vk_device, uv_stagingBuffer, &memReq);
    allocInfo.allocationSize = memReq.size;
    
    VkDeviceMemory uv_stagingMemory;
    vk_dev_funcs->vkAllocateMemory(vk_device, &allocInfo, nullptr, &uv_stagingMemory);
    vk_dev_funcs->vkBindBufferMemory(vk_device, uv_stagingBuffer, uv_stagingMemory, 0);
    
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = vk_command_pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    vk_dev_funcs->vkAllocateCommandBuffers(vk_device, &cmdAllocInfo, &cmdBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vk_dev_funcs->vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = res.y_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    vk_dev_funcs->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
    
    vk_dev_funcs->vkCmdCopyImageToBuffer(cmdBuffer, res.y_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, y_stagingBuffer, 1, &region);
    
    barrier.image = res.uv_image;
    vk_dev_funcs->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    region.imageExtent = {(uint32_t)(width / 2), (uint32_t)(height / 2), 1};
    vk_dev_funcs->vkCmdCopyImageToBuffer(cmdBuffer, res.uv_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, uv_stagingBuffer, 1, &region);
    
    vk_dev_funcs->vkEndCommandBuffer(cmdBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    vk_dev_funcs->vkQueueSubmit(vk_compute_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vk_dev_funcs->vkQueueWaitIdle(vk_compute_queue);
    
    void* data;
    vk_dev_funcs->vkMapMemory(vk_device, y_stagingMemory, 0, y_size, 0, &data);
    fwrite(data, 1, y_size, nv12_file);
    vk_dev_funcs->vkUnmapMemory(vk_device, y_stagingMemory);
    
    vk_dev_funcs->vkMapMemory(vk_device, uv_stagingMemory, 0, uv_size, 0, &data);
    fwrite(data, 1, uv_size, nv12_file);
    vk_dev_funcs->vkUnmapMemory(vk_device, uv_stagingMemory);
    fflush(nv12_file);
    
    vk_dev_funcs->vkFreeCommandBuffers(vk_device, vk_command_pool, 1, &cmdBuffer);
    vk_dev_funcs->vkFreeMemory(vk_device, y_stagingMemory, nullptr);
    vk_dev_funcs->vkFreeMemory(vk_device, uv_stagingMemory, nullptr);
    vk_dev_funcs->vkDestroyBuffer(vk_device, y_stagingBuffer, nullptr);
    vk_dev_funcs->vkDestroyBuffer(vk_device, uv_stagingBuffer, nullptr);
}
