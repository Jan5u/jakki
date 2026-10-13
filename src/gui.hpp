#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct User {
    std::string username;
};

struct Channel {
    std::string name;
    std::vector<User> users;
};

struct UIContext {
    SDL_Surface *surface = nullptr;
    TTF_Font *font = nullptr;
};

struct Button {
    SDL_FRect rect{};
    std::string label;
    SDL_Surface *icon = nullptr;
    float cornerRadius = 6.0f;
    SDL_Color background{60, 60, 60, 255};
    SDL_Color hoverBackground{85, 85, 85, 255};
    SDL_Color textColor{240, 240, 240, 255};
    bool hovered = false;
    bool held = false;

    bool handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

struct Slider {
    SDL_FRect rect{};
    float value = 0.5f;
    SDL_Color trackColor{50, 50, 50, 255};
    SDL_Color fillColor{90, 130, 200, 255};
    SDL_Color knobColor{230, 230, 230, 255};
    bool hovered = false;
    bool dragging = false;

    bool handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

struct TextInput {
    SDL_FRect rect{};
    std::string text;
    std::string placeholder;
    bool focused = false;
    bool hovered = false;
    int cursor = 0;
    int selectionAnchor = 0;
    bool selecting = false;
    TTF_Font *font = nullptr;
    float cornerRadius = 6.0f;
    float padding = 10.0f;
    SDL_Color background{32, 32, 38, 255};
    SDL_Color borderColor{70, 70, 76, 255};
    SDL_Color focusedBorderColor{90, 130, 200, 255};
    SDL_Color textColor{240, 240, 240, 255};
    SDL_Color placeholderColor{130, 130, 135, 255};
    SDL_Color cursorColor{230, 230, 230, 255};
    SDL_Color selectionColor{90, 130, 200, 140};

    bool handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;

    bool hasSelection() const { return cursor != selectionAnchor; }
    int selectionStart() const { return cursor < selectionAnchor ? cursor : selectionAnchor; }
    int selectionEnd() const { return cursor < selectionAnchor ? selectionAnchor : cursor; }
    void clearSelection() { selectionAnchor = cursor; }

    static void resetCursor();
    static void cleanupCursors();

  private:
    int indexAtX(TTF_Font *font, float x) const;
};

struct List {
    SDL_FRect rect{};
    std::vector<std::string> items;
    int selected = -1;
    int hovered = -1;
    float itemHeight = 24.0f;
    SDL_Color background{40, 40, 40, 255};
    SDL_Color hoverBackground{70, 70, 70, 255};
    SDL_Color selectedBackground{90, 130, 200, 255};
    SDL_Color textColor{240, 240, 240, 255};

    bool handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;

  private:
    int indexAt(float x, float y) const;
};

struct ChatMessage {
    std::string sender;
    std::string content;
};

struct ChannelListRow {
    int channel = -1;
    int user = -1;
    SDL_FRect rect{};
};

struct ChannelList {
    SDL_FRect rect{};
    std::vector<Channel> channels;
    int selectedChannel = -1;
    int selectedUser = -1;
    int hoveredRow = -1;

    float channelHeight = 30.0f;
    float userHeight = 26.0f;
    float userIndent = 22.0f;
    float avatarSize = 18.0f;
    float padding = 8.0f;
    float topPadding = 12.0f;
    float cornerRadius = 8.0f;
    float rowRadius = 4.0f;
    SDL_Surface *avatarIcon = nullptr;

    SDL_Color background{28, 28, 32, 255};
    SDL_Color channelHoverColor{52, 52, 60, 255};
    SDL_Color channelSelectedColor{64, 78, 110, 255};
    SDL_Color userHoverColor{60, 60, 68, 255};
    SDL_Color userSelectedColor{64, 78, 110, 255};
    SDL_Color channelTextColor{230, 230, 235, 255};
    SDL_Color textChannelColor{140, 150, 175, 255};
    SDL_Color userNameColor{205, 205, 212, 255};

    static bool isTextChannel(const std::string &name) { return !name.empty() && name[0] == '#'; }

    std::vector<ChannelListRow> buildRows() const;
    bool handleEvent(const SDL_Event &event, std::string &clickedChannel, std::string &clickedUser);
    void draw(const UIContext &ui) const;
};

struct ChatPanel {
    SDL_FRect rect{};
    bool open = false;
    bool sendRequested = false;
    std::string channel;
    std::vector<ChatMessage> messages;

    TextInput input;
    Button sendButton;

    float cornerRadius = 10.0f;
    float padding = 16.0f;
    float titleArea = 44.0f;
    float inputHeight = 36.0f;
    float inputGap = 12.0f;
    float buttonWidth = 90.0f;
    float lineHeight = 22.0f;

    SDL_Color background{38, 38, 43, 255};
    SDL_Color titleColor{240, 240, 240, 255};
    SDL_Color senderColor{110, 170, 240, 255};
    SDL_Color messageColor{220, 220, 225, 255};
    SDL_Color separatorColor{60, 60, 66, 255};
    SDL_Color emptyColor{140, 140, 148, 255};

    void layout();
    void handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

struct DialogSection {
    std::string title;
    SDL_Surface *icon = nullptr;
    SDL_FRect header{};
    bool hovered = false;
    std::vector<Slider> sliders;
};

struct Dialog {
    SDL_FRect rect{};
    bool open = false;
    std::string title;
    int selected = 0;

    float cornerRadius = 10.0f;
    float itemRadius = 8.0f;
    float padding = 24.0f;
    float titleArea = 56.0f;
    float iconSize = 20.0f;
    float sliderHeight = 20.0f;
    float sliderWidth = 0.0f;
    float contentGap = 12.0f;
    float navWidth = 180.0f;
    float navItemHeight = 40.0f;
    float navPadding = 12.0f;
    float navItemGap = 4.0f;

    SDL_Color dimColor{0, 0, 0, 170};
    SDL_Color background{45, 45, 50, 255};
    SDL_Color titleColor{240, 240, 240, 255};
    SDL_Color sectionColor{220, 220, 220, 255};
    SDL_Color separatorColor{70, 70, 76, 255};
    SDL_Color navHoverColor{65, 65, 72, 255};
    SDL_Color navSelectedColor{90, 130, 200, 255};

    std::vector<DialogSection> sections;

    void layout();
    void handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

struct ConnectDialog {
    SDL_FRect rect{};
    bool open = false;
    bool connectRequested = false;
    std::string title = "Connect";

    float cornerRadius = 10.0f;
    float padding = 24.0f;
    float titleArea = 56.0f;
    float labelHeight = 18.0f;
    float fieldHeight = 36.0f;
    float fieldGap = 18.0f;
    float buttonWidth = 120.0f;

    SDL_Color dimColor{0, 0, 0, 170};
    SDL_Color background{45, 45, 50, 255};
    SDL_Color titleColor{240, 240, 240, 255};
    SDL_Color labelColor{190, 190, 195, 255};

    TextInput address;
    TextInput username;
    Button connectButton;
    SDL_FRect addressLabel{};
    SDL_FRect usernameLabel{};

    void layout();
    void handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

struct ScreenshareDialog {
    SDL_FRect rect{};
    bool open = false;
    bool startRequested = false;
    std::string title = "Share Screen";
    std::string message = "Share your screen?";

    float cornerRadius = 10.0f;
    float padding = 24.0f;
    float titleArea = 56.0f;
    float messageHeight = 44.0f;
    float buttonWidth = 120.0f;
    float buttonHeight = 36.0f;
    float buttonGap = 12.0f;

    SDL_Color dimColor{0, 0, 0, 170};
    SDL_Color background{45, 45, 50, 255};
    SDL_Color titleColor{240, 240, 240, 255};
    SDL_Color messageColor{190, 190, 195, 255};

    Button startButton;
    Button cancelButton;
    SDL_FRect messageRect{};

    void layout();
    void handleEvent(const SDL_Event &event);
    void draw(const UIContext &ui) const;
};

class GUI {
  public:
    GUI() = default;
    void setOnStateChanged(std::function<void()> callback);
    void onChannelsReceived(const std::vector<Channel> &channels);
    void onUserJoinVoiceChannel(const std::string &userName, const std::string &channelName);
    void onUserLeaveVoiceChannel(const std::string &userName, const std::string &channelName);
    void onTextMessage(const std::string &channel, const std::string &sender, const std::string &content);
    void onHistoryReceived(const std::string &channel, const std::vector<ChatMessage> &messages);
    std::vector<Channel> getChannelList() const;
    std::vector<ChatMessage> getMessages(const std::string &channel) const;

    ChannelList channelList;

  private:
    std::unordered_map<std::string, std::vector<ChatMessage>> chatHistory;
    std::function<void()> onStateChanged;
    mutable std::mutex mutex;
    void clear();
    void notifyStateChanged();
};