#pragma once

#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QWidget>

#include <print>

#include "screen_renderer.hpp"

class VulkanWindow;

class VulkanWidget : public QWidget {
    Q_OBJECT

  public:
    explicit VulkanWidget(VulkanWindow *w);

  public slots:
    void onFrameQueued(int colorValue);

  private:
    VulkanWindow *m_window;
};

class VulkanRenderer : public ScreenRenderer {
  public:
    VulkanRenderer(VulkanWindow *w);

    void initResources() override;
    void startNextFrame() override;
};

class VulkanWindow : public QVulkanWindow {
    Q_OBJECT

  public:
    QVulkanWindowRenderer *createRenderer() override;

  signals:
    void frameQueued(int colorValue);
};