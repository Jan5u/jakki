#include "vulkan_renderer.hpp"

VulkanWidget::VulkanWidget(VulkanWindow *w) : m_window(w) {
    QWidget *wrapper = QWidget::createWindowContainer(w);
}

void VulkanWidget::onFrameQueued(int colorValue) {
    qDebug() << "onFrameQueued:" << colorValue; 
}

QVulkanWindowRenderer *VulkanWindow::createRenderer() {
    return new VulkanRenderer(this); 
}

extern ScreenRenderer *g_renderer;

VulkanRenderer::VulkanRenderer(VulkanWindow *w) : ScreenRenderer(w) {
    g_renderer = this;
    std::println("VulkanRenderer created, global pointer set");
}

void VulkanRenderer::initResources() {
    ScreenRenderer::initResources();

    QVulkanInstance *inst = m_window->vulkanInstance();
    m_devFuncs = inst->deviceFunctions(m_window->device());

    const int deviceCount = m_window->availablePhysicalDevices().count();
    std::println("Number of physical devices: {}", deviceCount);

    QVulkanFunctions *f = inst->functions();
    VkPhysicalDeviceProperties props;
    f->vkGetPhysicalDeviceProperties(m_window->physicalDevice(), &props);
    
    std::println("Active physical device: '{}'", props.deviceName);
    std::println("  Driver version: {}.{}.{}",
        VK_VERSION_MAJOR(props.driverVersion),
        VK_VERSION_MINOR(props.driverVersion),
        VK_VERSION_PATCH(props.driverVersion)
    );
    std::println("  API version: {}.{}.{}",
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion)
    );
    std::println("Supported instance layers:");
    for (const QVulkanLayer &layer : inst->supportedLayers()) {
        std::println("  {} v{}", layer.name.constData(), layer.version);
    }
    std::println("Enabled instance layers:");
    for (const QByteArray &layer : inst->layers()) {
        std::println("  {}", layer.constData());
    }
    std::println("Supported instance extensions:");
    for (const QVulkanExtension &ext : inst->supportedExtensions()) {
        std::println("  {} v{}", ext.name.constData(), ext.version);
    }
    std::println("Enabled instance extensions:");
    for (const QByteArray &ext : inst->extensions()) {
        std::println("  {}", ext.constData());
    }
    std::println("Color format: {}", static_cast<uint32_t>(m_window->colorFormat()));
    std::println("Depth-stencil format: {}", static_cast<uint32_t>(m_window->depthStencilFormat()));
    std::print("Supported sample counts:");
    const QList<int> sampleCounts = m_window->supportedSampleCounts();
    for (int count : sampleCounts) {
        std::print(" {}", count);
    }
    std::println("");
}

void VulkanRenderer::startNextFrame() {
    ScreenRenderer::startNextFrame();
    emit static_cast<VulkanWindow *>(m_window)->frameQueued(0);
}