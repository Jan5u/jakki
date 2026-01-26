#include "config.hpp"

Config::Config() {
    std::filesystem::path configDir;
    
#ifdef _WIN32
    const char *appData = std::getenv("APPDATA");
    if (appData && appData[0] != '\0') {
        configDir = std::filesystem::path(appData) / "jakki";
    } else {
        std::cerr << "APPDATA environment variable not set" << std::endl;
        return;
    }
#else
    const char *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfigHome && xdgConfigHome[0] != '\0') {
        configDir = std::filesystem::path(xdgConfigHome) / "jakki";
    } else {
        const char *home = std::getenv("HOME");
        if (!home) {
            std::cerr << "HOME environment variable not set\n";
            return;
        }
        configDir = std::filesystem::path(home) / ".config" / "jakki";
    }
#endif
    
    std::filesystem::create_directories(configDir);

    configPath = configDir / "config.ini";

    if (!std::filesystem::exists(configPath)) {
        createDefaultConfig();
    }

    load();
}

void Config::load() {
    if (!std::filesystem::exists(configPath)) {
        std::cout << "Config file not found: " << configPath << std::endl;
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << configPath << std::endl;
        return;
    }

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // Parse section [SectionName]
        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            currentSection = trim(currentSection);
            continue;
        }

        // Parse key=value
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = trim(line.substr(0, equalPos));
            std::string value = trim(line.substr(equalPos + 1));

            if (!currentSection.empty() && !key.empty()) {
                data[currentSection][key] = value;
            }
        }
    }

    std::cout << "Config loaded from: " << configPath << std::endl;
}

void Config::save() {
    std::ofstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "Failed to save config file: " << configPath << std::endl;
        return;
    }

    for (const auto &section : data) {
        file << "[" << section.first << "]\n";
        for (const auto &keyValue : section.second) {
            file << keyValue.first << "=" << keyValue.second << "\n";
        }
        file << "\n";
    }

    file.close();

    std::cout << "Config saved to: " << configPath << std::endl;
}

std::string Config::getInputDevice() const {
    auto sectionIt = data.find("Audio");
    if (sectionIt == data.end()) {
        return "";
    }

    auto keyIt = sectionIt->second.find("InputDevice");
    if (keyIt == sectionIt->second.end()) {
        return "";
    }

    return keyIt->second;
}

std::string Config::getOutputDevice() const {
    auto sectionIt = data.find("Audio");
    if (sectionIt == data.end()) {
        return "";
    }

    auto keyIt = sectionIt->second.find("OutputDevice");
    if (keyIt == sectionIt->second.end()) {
        return "";
    }

    return keyIt->second;
}

void Config::setInputDevice(const std::string &deviceId) {
    data["Audio"]["InputDevice"] = deviceId;
    save();
    std::cout << "Input device set to: " << deviceId << std::endl;
}

void Config::setOutputDevice(const std::string &deviceId) {
    data["Audio"]["OutputDevice"] = deviceId;
    save();
    std::cout << "Output device set to: " << deviceId << std::endl;
}

std::string Config::getTheme() const {
    auto sectionIt = data.find("Appearance");
    if (sectionIt == data.end()) {
        return "Default";
    }

    auto keyIt = sectionIt->second.find("Theme");
    if (keyIt == sectionIt->second.end()) {
        return "Default";
    }

    return keyIt->second;
}

void Config::setTheme(const std::string &theme) {
    data["Appearance"]["Theme"] = theme;
    save();
    std::cout << "Theme set to: " << theme << std::endl;
}

static std::string join(const std::vector<std::string>& vec, const char* delim = ",") {
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i != 0) oss << delim;
        oss << vec[i];
    }
    return oss.str();
}

static std::vector<std::string> split(const std::string& s, char delim = ',') {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) elems.push_back(item);
    }
    return elems;
}

void Config::setSupportedNVIDIAEncoders(const std::vector<std::string>& encoders) {
    data["Encoders"]["NVIDIA"] = join(encoders);
    save();
}

void Config::setSupportedVulkanEncoders(const std::vector<std::string>& encoders) {
    data["Encoders"]["Vulkan"] = join(encoders);
    save();
}

std::vector<std::string> Config::getSupportedNVIDIAEncoders() const {
    auto sectionIt = data.find("Encoders");
    if (sectionIt == data.end()) return {};
    auto keyIt = sectionIt->second.find("NVIDIA");
    if (keyIt == sectionIt->second.end()) return {};
    return split(keyIt->second);
}

std::vector<std::string> Config::getSupportedVulkanEncoders() const {
    auto sectionIt = data.find("Encoders");
    if (sectionIt == data.end()) return {};
    auto keyIt = sectionIt->second.find("Vulkan");
    if (keyIt == sectionIt->second.end()) return {};
    return split(keyIt->second);
}

void Config::createDefaultConfig() {
    std::ofstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "Failed to create config file: " << configPath << std::endl;
        return;
    }

    file << "[Audio]\n";
    file << "InputDevice=\n";
    file << "OutputDevice=\n";
    file << "\n";
    file << "[Appearance]\n";
    file << "Theme=Default\n";
    file << "\n";

    file.close();

    std::cout << "Created default config file: " << configPath << std::endl;
}

std::string Config::trim(const std::string &str) const {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}