#pragma once

#include <filesystem>
#include <map>
#include <string>

class Config {
  public:
    Config();

    void load();
    void save();

    std::filesystem::path getConfigPath() const { return configPath; }

    // Audio device getters
    std::string getInputDevice() const;
    std::string getOutputDevice() const;

    // Audio device setters
    void setInputDevice(const std::string &deviceId);
    void setOutputDevice(const std::string &deviceId);

  private:
    std::filesystem::path configPath;
    std::map<std::string, std::map<std::string, std::string>> data;

    void createDefaultConfig();
    std::string trim(const std::string &str) const;
};