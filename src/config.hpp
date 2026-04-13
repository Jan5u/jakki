#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

class Config {
  public:
    Config();
    void load();
    void save();
    std::filesystem::path getConfigPath() const { return configPath; }
    std::string getInputDevice() const;
    std::string getOutputDevice() const;
    void setInputDevice(const std::string &deviceId);
    void setOutputDevice(const std::string &deviceId);
    std::string getTheme() const;
    void setTheme(const std::string &theme);
    void setSupportedNVIDIAEncoders(const std::vector<std::string>& encoders);
    void setSupportedAMDEncoders(const std::vector<std::string>& encoders);
    void setSupportedVulkanEncoders(const std::vector<std::string>& encoders);
    std::vector<std::string> getSupportedNVIDIAEncoders() const;
    std::vector<std::string> getSupportedAMDEncoders() const;
    std::vector<std::string> getSupportedVulkanEncoders() const;
    std::string getPreferredDecoder() const;
    void setPreferredDecoder(const std::string &decoder);

  private:
    std::filesystem::path configPath;
    std::map<std::string, std::map<std::string, std::string>> data;
    void createDefaultConfig();
    std::string trim(const std::string &str) const;
};