#include "gui.hpp"
#include <algorithm>
#include <cmath>
#include <print>

namespace {

bool contains(const SDL_FRect &rect, float x, float y) { return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h; }

void fillRect(SDL_Surface *surface, const SDL_FRect &rect, SDL_Color color) {
    if (!surface) {
        return;
    }
    SDL_Rect r{(int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h};
    SDL_FillSurfaceRect(surface, &r, SDL_MapSurfaceRGBA(surface, color.r, color.g, color.b, color.a));
}

void fillRoundedRect(SDL_Surface *surface, const SDL_FRect &rect, float radius, SDL_Color color) {
    if (!surface || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    const int w = (int)rect.w;
    const int h = (int)rect.h;
    const int x0 = (int)rect.x;
    const int y0 = (int)rect.y;
    radius = SDL_clamp(radius, 0.0f, SDL_min((float)w, (float)h) * 0.5f);
    if (radius <= 0.0f) {
        fillRect(surface, rect, color);
        return;
    }

    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(surface->format);
    if (!format || format->bytes_per_pixel > (int)sizeof(Uint32)) {
        return;
    }

    const int bpp = format->bytes_per_pixel;
    const Uint32 opaque =
        ((Uint32)color.r << format->Rshift) | ((Uint32)color.g << format->Gshift) | ((Uint32)color.b << format->Bshift) | format->Amask;
    const int surfaceW = surface->w;
    const int surfaceH = surface->h;
    const int pitch = surface->pitch;

    if (SDL_MUSTLOCK(surface)) {
        SDL_LockSurface(surface);
    }
    Uint8 *base = (Uint8 *)surface->pixels;

    const int clipX0 = SDL_max(x0, 0);
    const int clipY0 = SDL_max(y0, 0);
    const int clipX1 = SDL_min(x0 + w, surfaceW);
    const int clipY1 = SDL_min(y0 + h, surfaceH);
    if (clipX1 <= clipX0 || clipY1 <= clipY0) {
        if (SDL_MUSTLOCK(surface)) {
            SDL_UnlockSurface(surface);
        }
        return;
    }

    auto coverageAt = [&](float px, float fy) -> float {
        const float fx = SDL_max(SDL_max(radius - px, px - (w - radius)), 0.0f);
        if (fx <= 0.0f) {
            return SDL_clamp(radius - fy + 0.5f, 0.0f, 1.0f);
        }
        if (fy <= 0.0f) {
            return SDL_clamp(radius - fx + 0.5f, 0.0f, 1.0f);
        }
        return SDL_clamp(radius - SDL_sqrtf(fx * fx + fy * fy) + 0.5f, 0.0f, 1.0f);
    };

    auto blendPixel = [&](int dstX, int dstY, float coverage) {
        Uint8 *dst = base + (size_t)dstY * pitch + (size_t)dstX * bpp;
        if (coverage >= 1.0f && color.a == 255) {
            SDL_memcpy(dst, &opaque, bpp);
            return;
        }
        const Uint32 srcA = (Uint32)(color.a * coverage + 0.5f);
        Uint32 dstPixel = 0;
        SDL_memcpy(&dstPixel, dst, bpp);
        const Uint32 dstA = format->Amask ? ((dstPixel & format->Amask) >> format->Ashift) : 255;
        const Uint32 outA = srcA + dstA * (255 - srcA) / 255;
        Uint32 outPixel = 0;
        if (outA > 0) {
            const Uint32 dstR = (dstPixel & format->Rmask) >> format->Rshift;
            const Uint32 dstG = (dstPixel & format->Gmask) >> format->Gshift;
            const Uint32 dstB = (dstPixel & format->Bmask) >> format->Bshift;
            const Uint32 r = (color.r * srcA + dstR * dstA * (255 - srcA) / 255) / outA;
            const Uint32 g = (color.g * srcA + dstG * dstA * (255 - srcA) / 255) / outA;
            const Uint32 b = (color.b * srcA + dstB * dstA * (255 - srcA) / 255) / outA;
            outPixel = ((r << format->Rshift) & format->Rmask) | ((g << format->Gshift) & format->Gmask) | ((b << format->Bshift) & format->Bmask) |
                       ((outA << format->Ashift) & format->Amask);
        }
        SDL_memcpy(dst, &outPixel, bpp);
    };

    if (color.a == 255 && bpp == (int)sizeof(Uint32)) {
        const int r = (int)SDL_ceilf(radius);
        const int bandTop = SDL_max(y0 + r, clipY0);
        const int bandBottom = SDL_min(y0 + h - r, clipY1);
        for (int yy = bandTop; yy < bandBottom; ++yy) {
            SDL_memset4((Uint32 *)(base + (size_t)yy * pitch) + clipX0, opaque, clipX1 - clipX0);
        }

        const int columnLeft = SDL_max(x0 + r, clipX0);
        const int columnRight = SDL_min(x0 + w - r, clipX1);
        if (columnRight > columnLeft) {
            for (int yy = SDL_max(y0, clipY0); yy < SDL_min(y0 + r, clipY1); ++yy) {
                SDL_memset4((Uint32 *)(base + (size_t)yy * pitch) + columnLeft, opaque, columnRight - columnLeft);
            }
            for (int yy = SDL_max(y0 + h - r, clipY0); yy < SDL_min(y0 + h, clipY1); ++yy) {
                SDL_memset4((Uint32 *)(base + (size_t)yy * pitch) + columnLeft, opaque, columnRight - columnLeft);
            }
        }

        for (int yy = clipY0; yy < clipY1; ++yy) {
            const float py = (yy - y0) + 0.5f;
            const float fy = SDL_max(SDL_max(radius - py, py - (h - radius)), 0.0f);
            const int leftEnd = SDL_min(x0 + r, clipX1);
            for (int xx = clipX0; xx < leftEnd; ++xx) {
                blendPixel(xx, yy, coverageAt((xx - x0) + 0.5f, fy));
            }
            const int rightStart = SDL_max(x0 + w - r, clipX0);
            for (int xx = rightStart; xx < clipX1; ++xx) {
                blendPixel(xx, yy, coverageAt((xx - x0) + 0.5f, fy));
            }
        }
    } else {
        for (int yy = clipY0; yy < clipY1; ++yy) {
            const float py = (yy - y0) + 0.5f;
            const float fy = SDL_max(SDL_max(radius - py, py - (h - radius)), 0.0f);
            for (int xx = clipX0; xx < clipX1; ++xx) {
                blendPixel(xx, yy, coverageAt((xx - x0) + 0.5f, fy));
            }
        }
    }

    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }
}

void glyphVerticalBounds(SDL_Surface *textSurface, int &first, int &last) {
    first = 0;
    last = textSurface->h - 1;
    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(textSurface->format);
    if (!format || !format->Amask) {
        return;
    }
    if (SDL_MUSTLOCK(textSurface)) {
        SDL_LockSurface(textSurface);
    }
    const Uint8 *base = (const Uint8 *)textSurface->pixels;
    const int pitch = textSurface->pitch;
    const int bpp = format->bytes_per_pixel;
    int f = textSurface->h;
    int l = -1;
    for (int y = 0; y < textSurface->h; ++y) {
        for (int x = 0; x < textSurface->w; ++x) {
            Uint32 pixel = 0;
            SDL_memcpy(&pixel, base + (size_t)y * pitch + (size_t)x * bpp, bpp);
            if (((pixel & format->Amask) >> format->Ashift) > 12) {
                if (y < f) {
                    f = y;
                }
                l = y;
                break;
            }
        }
    }
    if (SDL_MUSTLOCK(textSurface)) {
        SDL_UnlockSurface(textSurface);
    }
    if (l >= f) {
        first = f;
        last = l;
    }
}

void drawText(SDL_Surface *surface, TTF_Font *font, const std::string &text, SDL_Color color, float centerX, float centerY) {
    if (!surface || !font || text.empty()) {
        return;
    }
    SDL_Surface *textSurface = TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
    if (!textSurface) {
        return;
    }
    int first = 0, last = textSurface->h - 1;
    glyphVerticalBounds(textSurface, first, last);
    SDL_Rect dst{
        (int)(centerX - textSurface->w * 0.5f),
        (int)(centerY - (first + last) * 0.5f),
        textSurface->w,
        textSurface->h,
    };
    SDL_BlitSurface(textSurface, nullptr, surface, &dst);
    SDL_DestroySurface(textSurface);
}

void drawTextCenteredY(SDL_Surface *surface, TTF_Font *font, const std::string &text, SDL_Color color, float x, float centerY) {
    if (!surface || !font || text.empty()) {
        return;
    }
    SDL_Surface *textSurface = TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
    if (!textSurface) {
        return;
    }
    int first = 0, last = textSurface->h - 1;
    glyphVerticalBounds(textSurface, first, last);
    SDL_Rect dst{(int)x, (int)(centerY - (first + last) * 0.5f), textSurface->w, textSurface->h};
    SDL_BlitSurface(textSurface, nullptr, surface, &dst);
    SDL_DestroySurface(textSurface);
}

} // namespace

bool Button::handleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        hovered = contains(rect, event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT && contains(rect, event.button.x, event.button.y)) {
            held = true;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            bool clicked = held && contains(rect, event.button.x, event.button.y);
            held = false;
            return clicked;
        }
        break;
    }
    return false;
}

void Button::draw(const UIContext &ui) const {
    fillRoundedRect(ui.surface, rect, cornerRadius, (hovered || held) ? hoverBackground : background);
    if (icon) {
        SDL_Rect dst{
            (int)(rect.x + (rect.w - icon->w) * 0.5f),
            (int)(rect.y + (rect.h - icon->h) * 0.5f),
            icon->w,
            icon->h,
        };
        SDL_BlitSurface(icon, nullptr, ui.surface, &dst);
        return;
    }
    drawText(ui.surface, ui.font, label, textColor, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f);
}

bool Slider::handleEvent(const SDL_Event &event) {
    auto setValue = [&](float x) {
        float next = SDL_clamp((x - rect.x) / rect.w, 0.0f, 1.0f);
        if (next == value) {
            return false;
        }
        value = next;
        return true;
    };

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        hovered = contains(rect, event.motion.x, event.motion.y);
        if (dragging) {
            return setValue(event.motion.x);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT && contains(rect, event.button.x, event.button.y)) {
            dragging = true;
            SDL_CaptureMouse(true);
            return setValue(event.button.x);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (dragging) {
                SDL_CaptureMouse(false);
            }
            dragging = false;
        }
        break;
    }
    return false;
}

void Slider::draw(const UIContext &ui) const {
    fillRect(ui.surface, rect, trackColor);

    SDL_FRect filled{rect.x, rect.y, rect.w * value, rect.h};
    fillRect(ui.surface, filled, fillColor);

    const float knobWidth = 12.0f;
    SDL_FRect knob{rect.x + rect.w * value - knobWidth * 0.5f, rect.y - 4.0f, knobWidth, rect.h + 8.0f};
    fillRect(ui.surface, knob, knobColor);
}

static SDL_Cursor *g_textCursor = nullptr;
static SDL_Cursor *g_defaultCursor = nullptr;
static bool g_cursorsReady = false;
static int g_hoverCount = 0;

static bool isContinuation(char c) { return (c & 0xC0) == 0x80; }

static int prevBoundary(const std::string &s, int i) {
    if (i <= 0) {
        return 0;
    }
    --i;
    while (i > 0 && isContinuation(s[i])) {
        --i;
    }
    return i;
}

static int nextBoundary(const std::string &s, int i) {
    const int n = (int)s.size();
    if (i >= n) {
        return n;
    }
    ++i;
    while (i < n && isContinuation(s[i])) {
        ++i;
    }
    return i;
}

void TextInput::resetCursor() {
    g_hoverCount = 0;
    if (g_cursorsReady) {
        SDL_SetCursor(g_defaultCursor);
    }
}

void TextInput::cleanupCursors() {
    SDL_DestroyCursor(g_textCursor);
    SDL_DestroyCursor(g_defaultCursor);
    g_textCursor = nullptr;
    g_defaultCursor = nullptr;
    g_cursorsReady = false;
    g_hoverCount = 0;
}

int TextInput::indexAtX(TTF_Font *f, float x) const {
    if (!f || text.empty()) {
        return 0;
    }
    const float textX = rect.x + padding;
    const int n = (int)text.size();
    int bestIndex = 0;
    float bestDist = std::abs(textX - x);
    for (int i = 1; i <= n; ++i) {
        if (i < n && isContinuation(text[i])) {
            continue;
        }
        int w = 0, h = 0;
        if (!TTF_GetStringSize(f, text.c_str(), i, &w, &h)) {
            continue;
        }
        const float dist = std::abs(textX + (float)w - x);
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool TextInput::handleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const bool over = contains(rect, event.motion.x, event.motion.y);
        if (over != hovered) {
            hovered = over;
            g_hoverCount += over ? 1 : -1;
            if (g_hoverCount < 0) {
                g_hoverCount = 0;
            }
            if (!g_cursorsReady) {
                g_textCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
                g_defaultCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
                g_cursorsReady = true;
            }
            SDL_SetCursor(g_hoverCount > 0 ? g_textCursor : g_defaultCursor);
        }
        if (selecting && focused) {
            const int idx = indexAtX(font, event.motion.x);
            if (idx != cursor) {
                cursor = idx;
                return true;
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            const bool wasFocused = focused;
            const bool inside = contains(rect, event.button.x, event.button.y);
            focused = inside;
            if (inside) {
                cursor = indexAtX(font, event.button.x);
                selectionAnchor = cursor;
                selecting = true;
            } else {
                selecting = false;
            }
            if (focused != wasFocused || inside) {
                return true;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            selecting = false;
        }
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (!focused) {
            break;
        }
        const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
        switch (event.key.key) {
        case SDLK_BACKSPACE:
            if (hasSelection()) {
                const int s = selectionStart();
                text.erase(s, selectionEnd() - s);
                cursor = selectionAnchor = s;
            } else if (cursor > 0) {
                const int p = prevBoundary(text, cursor);
                text.erase(p, cursor - p);
                cursor = selectionAnchor = p;
            } else {
                break;
            }
            return true;
        case SDLK_DELETE:
            if (hasSelection()) {
                const int s = selectionStart();
                text.erase(s, selectionEnd() - s);
                cursor = selectionAnchor = s;
            } else if (cursor < (int)text.size()) {
                text.erase(cursor, nextBoundary(text, cursor) - cursor);
            } else {
                break;
            }
            return true;
        case SDLK_LEFT:
            cursor = (hasSelection() && !shift) ? selectionStart() : prevBoundary(text, cursor);
            if (!shift) {
                selectionAnchor = cursor;
            }
            return true;
        case SDLK_RIGHT:
            cursor = (hasSelection() && !shift) ? selectionEnd() : nextBoundary(text, cursor);
            if (!shift) {
                selectionAnchor = cursor;
            }
            return true;
        case SDLK_HOME:
            cursor = 0;
            if (!shift) {
                selectionAnchor = cursor;
            }
            return true;
        case SDLK_END:
            cursor = (int)text.size();
            if (!shift) {
                selectionAnchor = cursor;
            }
            return true;
        case SDLK_A:
            if (ctrl) {
                selectionAnchor = 0;
                cursor = (int)text.size();
                return true;
            }
            break;
        default:
            break;
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (focused && event.text.text && event.text.text[0] != '\0') {
            if (hasSelection()) {
                const int s = selectionStart();
                text.erase(s, selectionEnd() - s);
                cursor = selectionAnchor = s;
            }
            const std::string inserted = event.text.text;
            text.insert(cursor, inserted);
            cursor += (int)inserted.size();
            selectionAnchor = cursor;
            return true;
        }
        break;
    }
    return false;
}

void TextInput::draw(const UIContext &ui) const {
    if (!ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, rect, cornerRadius, focused ? focusedBorderColor : borderColor);
    const SDL_FRect inner{rect.x + 1.0f, rect.y + 1.0f, rect.w - 2.0f, rect.h - 2.0f};
    fillRoundedRect(ui.surface, inner, SDL_max(cornerRadius - 1.0f, 0.0f), background);

    const float centerY = rect.y + rect.h * 0.5f;
    const float textX = rect.x + padding;
    TTF_Font *f = ui.font ? ui.font : font;

    SDL_Rect clip{(int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h};
    SDL_Rect previous{};
    const bool hadClip = SDL_GetSurfaceClipRect(ui.surface, &previous);
    SDL_SetSurfaceClipRect(ui.surface, &clip);

    const int textSize = (int)text.size();
    const int cur = std::clamp(cursor, 0, textSize);
    const int selStart = std::clamp(selectionStart(), 0, textSize);
    const int selEnd = std::clamp(selectionEnd(), 0, textSize);

    if (focused && selStart != selEnd && f) {
        auto prefixWidth = [&](int idx) {
            if (idx <= 0) {
                return 0.0f;
            }
            int w = 0, h = 0;
            TTF_GetStringSize(f, text.c_str(), idx, &w, &h);
            return (float)w;
        };
        const float x1 = textX + prefixWidth(selStart);
        const float x2 = textX + prefixWidth(selEnd);
        fillRect(ui.surface, {x1, rect.y + padding * 0.5f, x2 - x1, rect.h - padding}, selectionColor);
    }

    if (text.empty() && !placeholder.empty() && !focused) {
        drawTextCenteredY(ui.surface, ui.font, placeholder, placeholderColor, textX, centerY);
    } else if (!text.empty()) {
        drawTextCenteredY(ui.surface, ui.font, text, textColor, textX, centerY);
    }

    if (focused && f) {
        int w = 0, h = 0;
        if (cur > 0) {
            TTF_GetStringSize(f, text.c_str(), cur, &w, &h);
        }
        const float cursorHeight = rect.h - padding * 1.2f;
        fillRect(ui.surface, {textX + (float)w, centerY - cursorHeight * 0.5f, 1.5f, cursorHeight}, cursorColor);
    }

    SDL_SetSurfaceClipRect(ui.surface, hadClip ? &previous : nullptr);
}

int List::indexAt(float x, float y) const {
    if (!contains(rect, x, y) || itemHeight <= 0.0f) {
        return -1;
    }
    int index = (int)((y - rect.y) / itemHeight);
    if (index < 0 || index >= (int)items.size()) {
        return -1;
    }
    return index;
}

bool List::handleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        hovered = indexAt(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            int index = indexAt(event.button.x, event.button.y);
            if (index >= 0 && index != selected) {
                selected = index;
                return true;
            }
        }
        break;
    }
    return false;
}

void List::draw(const UIContext &ui) const {
    fillRect(ui.surface, rect, background);

    for (int i = 0; i < (int)items.size(); ++i) {
        SDL_FRect row{rect.x, rect.y + i * itemHeight, rect.w, itemHeight};
        if (row.y >= rect.y + rect.h) {
            break;
        }
        if (i == selected) {
            fillRect(ui.surface, row, selectedBackground);
        } else if (i == hovered) {
            fillRect(ui.surface, row, hoverBackground);
        }

        drawTextCenteredY(ui.surface, ui.font, items[i], textColor, row.x + 6.0f, row.y + row.h * 0.5f);
    }
}

std::vector<ChannelListRow> ChannelList::buildRows() const {
    std::vector<ChannelListRow> rows;
    const float x = rect.x + padding;
    const float rowWidth = rect.w - padding * 2.0f;
    float y = rect.y + topPadding;
    for (int ci = 0; ci < (int)channels.size(); ++ci) {
        rows.push_back({ci, -1, {x, y, rowWidth, channelHeight}});
        y += channelHeight;
        if (!isTextChannel(channels[ci].name)) {
            for (int ui = 0; ui < (int)channels[ci].users.size(); ++ui) {
                rows.push_back({ci, ui, {x, y, rowWidth, userHeight}});
                y += userHeight;
            }
        }
    }
    return rows;
}

bool ChannelList::handleEvent(const SDL_Event &event, std::string &clickedChannel, std::string &clickedUser) {
    clickedChannel.clear();
    clickedUser.clear();

    const std::vector<ChannelListRow> rows = buildRows();

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        int newHover = -1;
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (contains(rows[i].rect, event.motion.x, event.motion.y)) {
                newHover = i;
                break;
            }
        }
        if (newHover != hoveredRow) {
            hoveredRow = newHover;
            return true;
        }
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        for (const auto &row : rows) {
            if (!contains(row.rect, event.button.x, event.button.y)) {
                continue;
            }
            selectedChannel = row.channel;
            selectedUser = row.user;
            clickedChannel = channels[row.channel].name;
            if (row.user >= 0) {
                clickedUser = channels[row.channel].users[row.user].username;
            }
            return true;
        }
    }
    return false;
}

void ChannelList::draw(const UIContext &ui) const {
    if (!ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, rect, cornerRadius, background);

    SDL_Rect clip{(int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h};
    SDL_Rect previous{};
    const bool hadClip = SDL_GetSurfaceClipRect(ui.surface, &previous);
    SDL_SetSurfaceClipRect(ui.surface, &clip);

    const std::vector<ChannelListRow> rows = buildRows();
    for (int i = 0; i < (int)rows.size(); ++i) {
        const ChannelListRow &row = rows[i];
        if (row.rect.y >= rect.y + rect.h) {
            break;
        }
        const Channel &channel = channels[row.channel];
        if (row.user < 0) {
            if (row.channel == selectedChannel && selectedUser < 0) {
                fillRoundedRect(ui.surface, row.rect, rowRadius, channelSelectedColor);
            } else if (i == hoveredRow) {
                fillRoundedRect(ui.surface, row.rect, rowRadius, channelHoverColor);
            }
            const SDL_Color color = isTextChannel(channel.name) ? textChannelColor : channelTextColor;
            drawTextCenteredY(ui.surface, ui.font, channel.name, color, row.rect.x + 12.0f, row.rect.y + row.rect.h * 0.5f);
        } else {
            if (row.channel == selectedChannel && row.user == selectedUser) {
                fillRoundedRect(ui.surface, row.rect, rowRadius, userSelectedColor);
            } else if (i == hoveredRow) {
                fillRoundedRect(ui.surface, row.rect, rowRadius, userHoverColor);
            }
            const float x = row.rect.x + userIndent;
            if (avatarIcon) {
                SDL_Rect dst{
                    (int)x,
                    (int)(row.rect.y + (row.rect.h - avatarSize) * 0.5f),
                    (int)avatarSize,
                    (int)avatarSize,
                };
                SDL_BlitSurface(avatarIcon, nullptr, ui.surface, &dst);
            }
            drawTextCenteredY(ui.surface, ui.font, channel.users[row.user].username, userNameColor, x + avatarSize + 8.0f,
                              row.rect.y + row.rect.h * 0.5f);
        }
    }

    SDL_SetSurfaceClipRect(ui.surface, hadClip ? &previous : nullptr);
}

void Dialog::layout() {
    float navX = rect.x + navPadding;
    float navY = rect.y + titleArea;
    for (auto &section : sections) {
        section.header = {navX, navY, navWidth - navPadding * 2.0f, navItemHeight};
        navY += navItemHeight + navItemGap;
    }

    const float contentX = rect.x + navWidth + padding;
    float contentY = rect.y + titleArea;
    if (selected >= 0 && selected < (int)sections.size()) {
        const float available = rect.w - (navWidth + padding) - padding;
        const float width = sliderWidth > 0.0f ? std::min(sliderWidth, available) : available;
        for (auto &slider : sections[selected].sliders) {
            slider.rect = {contentX, contentY, width, sliderHeight};
            contentY += sliderHeight + contentGap;
        }
    }
}

void Dialog::handleEvent(const SDL_Event &event) {
    if (!open) {
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        open = false;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        for (auto &section : sections) {
            section.hovered = contains(section.header, event.motion.x, event.motion.y);
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (!contains(rect, event.button.x, event.button.y)) {
            open = false;
            return;
        }
        for (int i = 0; i < (int)sections.size(); ++i) {
            if (contains(sections[i].header, event.button.x, event.button.y)) {
                selected = i;
                layout();
                return;
            }
        }
    }
    if (selected >= 0 && selected < (int)sections.size()) {
        for (auto &slider : sections[selected].sliders) {
            slider.handleEvent(event);
        }
    }
}

void Dialog::draw(const UIContext &ui) const {
    if (!open || !ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, {0.0f, 0.0f, rect.w, rect.h}, cornerRadius, background);

    drawTextCenteredY(ui.surface, ui.font, title, titleColor, padding, titleArea * 0.5f);

    fillRect(ui.surface, {navWidth, titleArea, 1.0f, rect.h - titleArea}, separatorColor);

    for (int i = 0; i < (int)sections.size(); ++i) {
        const auto &section = sections[i];
        const SDL_FRect item{section.header.x - rect.x, section.header.y - rect.y, section.header.w, section.header.h};

        if (i == selected) {
            fillRoundedRect(ui.surface, item, itemRadius, navSelectedColor);
        } else if (section.hovered) {
            fillRoundedRect(ui.surface, item, itemRadius, navHoverColor);
        }

        if (section.icon) {
            SDL_Rect iconDst{
                (int)(item.x + 12.0f),
                (int)(item.y + (item.h - iconSize) * 0.5f),
                (int)iconSize,
                (int)iconSize,
            };
            SDL_BlitSurface(section.icon, nullptr, ui.surface, &iconDst);
        }

        drawTextCenteredY(ui.surface, ui.font, section.title, sectionColor, item.x + 12.0f + iconSize + 12.0f, item.y + item.h * 0.5f);
    }

    if (selected >= 0 && selected < (int)sections.size()) {
        for (const auto &slider : sections[selected].sliders) {
            Slider local = slider;
            local.rect.x -= rect.x;
            local.rect.y -= rect.y;
            local.draw(ui);
        }
    }
}

void ConnectDialog::layout() {
    const float contentX = rect.x + padding;
    const float contentWidth = rect.w - padding * 2.0f;
    float y = rect.y + titleArea + fieldGap;

    addressLabel = {contentX, y, contentWidth, labelHeight};
    y += labelHeight;
    address.rect = {contentX, y, contentWidth, fieldHeight};
    y += fieldHeight + fieldGap;

    usernameLabel = {contentX, y, contentWidth, labelHeight};
    y += labelHeight;
    username.rect = {contentX, y, contentWidth, fieldHeight};

    connectButton.rect = {
        rect.x + rect.w - padding - buttonWidth,
        rect.y + rect.h - padding - fieldHeight,
        buttonWidth,
        fieldHeight,
    };
}

void ConnectDialog::handleEvent(const SDL_Event &event) {
    if (!open) {
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        open = false;
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN) {
        connectRequested = true;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (!contains(rect, event.button.x, event.button.y)) {
            open = false;
            return;
        }
    }

    address.handleEvent(event);
    username.handleEvent(event);
    if (connectButton.handleEvent(event)) {
        connectRequested = true;
    }
}

void ConnectDialog::draw(const UIContext &ui) const {
    if (!open || !ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, {0.0f, 0.0f, rect.w, rect.h}, cornerRadius, background);
    drawTextCenteredY(ui.surface, ui.font, title, titleColor, padding, titleArea * 0.5f);

    drawTextCenteredY(ui.surface, ui.font, "IP Address", labelColor, addressLabel.x - rect.x, addressLabel.y - rect.y + labelHeight * 0.5f);
    drawTextCenteredY(ui.surface, ui.font, "Username", labelColor, usernameLabel.x - rect.x, usernameLabel.y - rect.y + labelHeight * 0.5f);

    TextInput addressLocal = address;
    addressLocal.rect.x -= rect.x;
    addressLocal.rect.y -= rect.y;
    addressLocal.draw(ui);

    TextInput usernameLocal = username;
    usernameLocal.rect.x -= rect.x;
    usernameLocal.rect.y -= rect.y;
    usernameLocal.draw(ui);

    Button buttonLocal = connectButton;
    buttonLocal.rect.x -= rect.x;
    buttonLocal.rect.y -= rect.y;
    buttonLocal.draw(ui);
}

void ScreenshareDialog::layout() {
    const float contentX = rect.x + padding;
    const float contentWidth = rect.w - padding * 2.0f;
    messageRect = {contentX, rect.y + titleArea, contentWidth, messageHeight};

    const float buttonsY = rect.y + rect.h - padding - buttonHeight;
    const float totalWidth = buttonWidth * 2.0f + buttonGap;
    startButton.rect = {contentX + contentWidth - totalWidth, buttonsY, buttonWidth, buttonHeight};
    cancelButton.rect = {contentX + contentWidth - buttonWidth, buttonsY, buttonWidth, buttonHeight};
}

void ScreenshareDialog::handleEvent(const SDL_Event &event) {
    if (!open) {
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        open = false;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (!contains(rect, event.button.x, event.button.y)) {
            open = false;
            return;
        }
    }

    if (startButton.handleEvent(event)) {
        startRequested = true;
    }
    if (cancelButton.handleEvent(event)) {
        open = false;
    }
}

void ScreenshareDialog::draw(const UIContext &ui) const {
    if (!open || !ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, {0.0f, 0.0f, rect.w, rect.h}, cornerRadius, background);
    drawTextCenteredY(ui.surface, ui.font, title, titleColor, padding, titleArea * 0.5f);
    drawTextCenteredY(ui.surface, ui.font, message, messageColor, padding, (messageRect.y - rect.y) + messageHeight * 0.5f);

    Button startLocal = startButton;
    startLocal.rect.x -= rect.x;
    startLocal.rect.y -= rect.y;
    startLocal.draw(ui);

    Button cancelLocal = cancelButton;
    cancelLocal.rect.x -= rect.x;
    cancelLocal.rect.y -= rect.y;
    cancelLocal.draw(ui);
}

void ChatPanel::layout() {
    const float contentX = rect.x + padding;
    const float contentWidth = rect.w - padding * 2.0f;
    const float inputY = rect.y + rect.h - padding - inputHeight;
    sendButton.rect = {contentX + contentWidth - buttonWidth, inputY, buttonWidth, inputHeight};
    input.rect = {contentX, inputY, contentWidth - buttonWidth - inputGap, inputHeight};
}

void ChatPanel::handleEvent(const SDL_Event &event) {
    if (!open) {
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        open = false;
        return;
    }

    input.handleEvent(event);
    if (sendButton.handleEvent(event)) {
        sendRequested = true;
    }
}

void ChatPanel::draw(const UIContext &ui) const {
    if (!open || !ui.surface) {
        return;
    }

    fillRoundedRect(ui.surface, {0.0f, 0.0f, rect.w, rect.h}, cornerRadius, background);
    drawTextCenteredY(ui.surface, ui.font, channel, titleColor, padding, titleArea * 0.5f);
    fillRect(ui.surface, {0.0f, titleArea, rect.w, 1.0f}, separatorColor);

    const float inputY = rect.h - padding - inputHeight;
    const float messageTop = titleArea + padding;
    const float messageBottom = inputY - inputGap;

    SDL_Rect clip{0, (int)messageTop, (int)rect.w, (int)std::max(messageBottom - messageTop, 0.0f)};
    SDL_Rect previous{};
    const bool hadClip = SDL_GetSurfaceClipRect(ui.surface, &previous);
    SDL_SetSurfaceClipRect(ui.surface, &clip);

    if (messages.empty()) {
        drawTextCenteredY(ui.surface, ui.font, "No messages yet", emptyColor, padding, messageTop + lineHeight * 0.5f);
    } else {
        float bottom = messageBottom;
        for (int i = (int)messages.size() - 1; i >= 0; --i) {
            const float y = bottom - lineHeight;
            if (y < messageTop) {
                break;
            }
            const ChatMessage &message = messages[i];
            const float centerY = y + lineHeight * 0.5f;
            if (!message.sender.empty() && ui.font) {
                int senderWidth = 0, senderHeight = 0;
                TTF_GetStringSize(ui.font, message.sender.c_str(), message.sender.size(), &senderWidth, &senderHeight);
                drawTextCenteredY(ui.surface, ui.font, message.sender, senderColor, padding, centerY);
                drawTextCenteredY(ui.surface, ui.font, ": " + message.content, messageColor, padding + (float)senderWidth, centerY);
            } else {
                drawTextCenteredY(ui.surface, ui.font, message.content, messageColor, padding, centerY);
            }
            bottom -= lineHeight;
        }
    }

    SDL_SetSurfaceClipRect(ui.surface, hadClip ? &previous : nullptr);

    TextInput inputLocal = input;
    inputLocal.rect.x -= rect.x;
    inputLocal.rect.y -= rect.y;
    inputLocal.draw(ui);

    Button buttonLocal = sendButton;
    buttonLocal.rect.x -= rect.x;
    buttonLocal.rect.y -= rect.y;
    buttonLocal.draw(ui);
}

void GUI::setOnStateChanged(std::function<void()> callback) { onStateChanged = std::move(callback); }

void GUI::notifyStateChanged() {
    if (onStateChanged) {
        onStateChanged();
    }
}

void GUI::onChannelsReceived(const std::vector<Channel> &chs) {
    {
        std::lock_guard lock(mutex);
        clear();
        for (const auto &c : chs) {
            if (c.name.empty()) {
                continue;
            }
            channelList.channels.push_back(c);
        }
    }
    notifyStateChanged();
}

std::vector<Channel> GUI::getChannelList() const {
    std::lock_guard lock(mutex);
    return channelList.channels;
}

void GUI::clear() {
    channelList.channels.clear();
    channelList.selectedChannel = -1;
    channelList.selectedUser = -1;
    channelList.hoveredRow = -1;
}

void GUI::onUserJoinVoiceChannel(const std::string &userName, const std::string &channelName) {
    if (channelName.empty() || userName.empty()) {
        return;
    }

    {
        std::lock_guard lock(mutex);
        auto it = std::find_if(channelList.channels.begin(), channelList.channels.end(),
                               [&](const Channel &channel) { return channel.name == channelName; });
        if (it == channelList.channels.end()) {
            channelList.channels.push_back(Channel{channelName, {}});
            it = channelList.channels.end() - 1;
        }

        const auto userIt = std::find_if(it->users.begin(), it->users.end(), [&](const User &user) { return user.username == userName; });
        if (userIt == it->users.end()) {
            it->users.push_back(User{userName});
        }
    }
    notifyStateChanged();
}

void GUI::onUserLeaveVoiceChannel(const std::string &userName, const std::string &channelName) {
    if (channelName.empty() || userName.empty()) {
        return;
    }

    {
        std::lock_guard lock(mutex);
        auto it = std::find_if(channelList.channels.begin(), channelList.channels.end(),
                               [&](const Channel &channel) { return channel.name == channelName; });
        if (it == channelList.channels.end()) {
            return;
        }

        const auto userIt = std::find_if(it->users.begin(), it->users.end(), [&](const User &user) { return user.username == userName; });
        if (userIt == it->users.end()) {
            return;
        }

        const int removed = (int)std::distance(it->users.begin(), userIt);
        it->users.erase(userIt);
        if (channelList.selectedChannel >= 0 && channelList.selectedChannel < (int)channelList.channels.size() &&
            channelList.channels[channelList.selectedChannel].name == channelName && channelList.selectedUser == removed) {
            channelList.selectedUser = -1;
        }
    }
    notifyStateChanged();
}

void GUI::onTextMessage(const std::string &channel, const std::string &sender, const std::string &content) {
    if (channel.empty()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        chatHistory[channel].push_back(ChatMessage{sender, content});
    }
    notifyStateChanged();
}

void GUI::onHistoryReceived(const std::string &channel, const std::vector<ChatMessage> &messages) {
    if (channel.empty()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        chatHistory[channel] = messages;
    }
    notifyStateChanged();
}

std::vector<ChatMessage> GUI::getMessages(const std::string &channel) const {
    std::lock_guard lock(mutex);
    const auto it = chatHistory.find(channel);
    if (it == chatHistory.end()) {
        return {};
    }
    return it->second;
}
