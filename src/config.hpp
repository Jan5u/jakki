#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

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

  private:
    std::filesystem::path configPath;
    std::map<std::string, std::map<std::string, std::string>> data;
    void createDefaultConfig();
    std::string trim(const std::string &str) const;
};