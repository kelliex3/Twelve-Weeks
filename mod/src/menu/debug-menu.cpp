#include "debug-menu.hpp"
#include "engine/script.hpp"
#include "engine/draw.hpp"
#include <sstream>
#include <algorithm>

// Global Constants Definitions
const int SELECTED_COLOR = _rgba(255, 255, 255, 255);
const int DEFAULT_COLOR = _rgba(224, 224, 224, 165);
const int BLUE_SELECTED = _rgba(0, 208, 255, 255);
const int FEEDBACK_FLASH_COLOR = _rgba(255, 255, 255, 100);

// MenuManager Class Implementation
MenuManager::MenuManager() : currentMenu(nullptr), menuActionFeedback(false), menuActionFeedbackTimer(0) {}

void MenuManager::PushMenu(std::unique_ptr<Menu> menu) {
    if (currentMenu) {
        menuStack.emplace_back(std::move(currentMenu));
    }
    currentMenu = std::move(menu);
}

bool MenuManager::PopMenu() {
    if (!menuStack.empty()) {
        currentMenu = std::move(menuStack.back());
        menuStack.pop_back();
        return true;
    }
    return false;
}


bool MenuManager::IsMenuInStack(const Menu& menu) const {
    if (currentMenu.get() == &menu) {
        return true;
    }

    return std::any_of(menuStack.begin(), menuStack.end(), [menu_ptr = &menu] (auto& item) { return item.get() == menu_ptr; });
}

void MenuManager::SetCurrentMenu(std::unique_ptr<Menu> menu) {
    // Clear the stack and set new current menu
    menuStack.clear();
    currentMenu = std::move(menu);
}

void MenuManager::TriggerActionFeedback() {
    menuActionFeedback = true;
    menuActionFeedbackTimer = 8;
}

bool MenuManager::IsActionFeedbackActive() const {
    return menuActionFeedback;
}

void MenuManager::UpdateActionFeedback() {
    if (menuActionFeedback) {
        menuActionFeedbackTimer--;
        if (menuActionFeedbackTimer <= 0) {
            menuActionFeedback = false;
        }
    }
}

bool MenuManager::IsEmpty() {
    return menuStack.empty();
}

void MenuManager::HandleInput() {
    if (!currentMenu) return;

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-down"))) {
        currentMenu->SelectNext();
        AUDIO::PLAY_SOUND(SID("sfx-cycle"));
    }

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-up"))) {
        currentMenu->SelectPrevious();
        AUDIO::PLAY_SOUND(SID("sfx-cycle"));
    }

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-left"))) {
        currentMenu->HandleLeftInput();
        AUDIO::PLAY_SOUND(SID("sfx-cycle"));
    }

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-right"))) {
        currentMenu->HandleRightInput();
        AUDIO::PLAY_SOUND(SID("sfx-cycle"));
    }

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-select"))) {
        currentMenu->HandleSelectInput();
        AUDIO::PLAY_SOUND(SID("sfx-change"));
    }

    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-back"))) {
        if (!PopMenu()) {
            // Handle what happens when we can't go back further
            // This could close the menu entirely or do nothing
        }
        else {
            AUDIO::PLAY_SOUND(SID("sfx-back"));
        }
    }
}

void MenuManager::Render() {
    if (currentMenu) {
        currentMenu->Render(*this);
    }
    UpdateActionFeedback();
}

std::string MenuManager::GetSelectedItemName() const {
    MenuItem* selectedItem = GetSelectedMenuItem();
    return selectedItem ? selectedItem->GetName() : "";
}

MenuItem* MenuManager::GetSelectedMenuItem() const {
    if (!currentMenu) return nullptr;
    int index = currentMenu->GetSelectedIndex();
    const auto& items = currentMenu->GetMenuItems();
    if (index >= 0 && index < items.size()) {
        return items[index].get();
    }
    return nullptr;
}


// Utility function to wrap text - used for handling description text
std::vector<std::string> WrapText(const std::string& text, float maxWidth, float fontSize) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string word, line;

    float lineWidth = 0.0f;

    while (stream >> word) {
        vec2 wordSize = DebugDrawCommon::GetTextWidthHeight(word.c_str(), fontSize);

        if (lineWidth + wordSize.x > maxWidth) {
            // Start a new line if we exceed the width
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
                lineWidth = 0.0f;
            }

            // If a single word is longer than max width, it needs to be split
            if (wordSize.x > maxWidth) {
                std::string partialWord;
                for (char c : word) {
                    partialWord += c;
                    vec2 partialSize = DebugDrawCommon::GetTextWidthHeight(partialWord.c_str(), fontSize);
                    if (partialSize.x > maxWidth) {
                        lines.push_back(partialWord.substr(0, partialWord.size() - 1));
                        partialWord = c;
                    }
                }
                if (!partialWord.empty()) {
                    line = partialWord + " ";
                    lineWidth = DebugDrawCommon::GetTextWidthHeight(line.c_str(), fontSize).x;
                }
            }
            else {
                line = word + " ";
                lineWidth = wordSize.x + DebugDrawCommon::GetTextWidthHeight(" ", fontSize).x;
            }
        }
        else {
            // Add word to the current line
            line += word + " ";
            lineWidth += wordSize.x + DebugDrawCommon::GetTextWidthHeight(" ", fontSize).x;
        }
    }

    // Add the last line if it's not empty
    if (!line.empty()) {
        lines.push_back(line);
    }

    return lines;
}

// Helper for MenuItem to align text to the right
float MenuItem::GetRightAlignedX(const std::string& text, float menuWidth, float fontSize, float padding) const {
    // Calculate position for right alignment with proper padding
    float textWidth = DebugDrawCommon::GetTextWidthHeight(text.c_str(), fontSize).x;
    return 110 + menuWidth - padding - textWidth;
}

// Helper for Menu to calculate text width
float Menu::CalculateTextWidth(const std::string& text, float fontSize) const {
    return DebugDrawCommon::GetTextWidthHeight(text.c_str(), fontSize).x;
}

// MenuItem Class Implementations
MenuItem::MenuItem(const std::string& label, const std::string& description)
    : label(label), description(description) {
}

// FloatScrollerMenuItem Class Implementations
FloatScrollerMenuItem::FloatScrollerMenuItem(const std::string& label, float& value, float minValue, float maxValue, float step, std::function<void()> action, const std::string& description)
    : MenuItem(label, description), value(value), minValue(minValue), maxValue(maxValue), step(step), action(action) {
}

void FloatScrollerMenuItem::OnSelect(MenuManager& manager) {
    action();
    manager.TriggerActionFeedback();
}

void FloatScrollerMenuItem::OnSelectLeft() {
    value -= step;
    if (value < minValue) value = minValue;
}

void FloatScrollerMenuItem::OnSelectRight() {
    value += step;
    if (value > maxValue) value = maxValue;
}

bool FloatScrollerMenuItem::IsScrollable() {
    return true;
}

void FloatScrollerMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;

    // Draw the label aligned to the left
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());

    // Draw the value aligned to the right
    std::string valueText = std::to_string(value);
    // Format float to 2 decimal places
    valueText = valueText.substr(0, valueText.find('.') + 3);

    // Calculate position for right alignment with proper padding
    float textX = 110 + menuWidth - theme.rightPadding -
        DebugDrawCommon::GetTextWidthHeight(valueText.c_str(), theme.itemFontSize).x;

    DebugDrawCommon::DrawTextPrintV(textX, y_position, theme.itemFontSize, color, valueText.c_str());
}

// IntScrollerMenuItem Class Implementations
IntScrollerMenuItem::IntScrollerMenuItem(const std::string& label, int& value, int minValue, int maxValue, int step, std::function<void()> action, const std::string& description)
    : MenuItem(label, description), value(value), minValue(minValue), maxValue(maxValue), step(step), action(action) {
}

void IntScrollerMenuItem::OnSelect(MenuManager& manager) {
    action();
    manager.TriggerActionFeedback();
}

void IntScrollerMenuItem::OnSelectLeft() {
    value -= step;
    if (value < minValue) value = minValue;
}

void IntScrollerMenuItem::OnSelectRight() {
    value += step;
    if (value > maxValue) value = maxValue;
}

void IntScrollerMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;

    // Draw the label aligned to the left
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());

    // Draw the value aligned to the right
    std::string valueText = std::to_string(value);

    // Calculate position for right alignment with proper padding
    float textX = 110 + menuWidth - theme.rightPadding -
        DebugDrawCommon::GetTextWidthHeight(valueText.c_str(), theme.itemFontSize).x;

    DebugDrawCommon::DrawTextPrintV(textX, y_position, theme.itemFontSize, color, valueText.c_str());
}

bool IntScrollerMenuItem::IsScrollable() {
    return true;
}

// OptionScrollerMenuItem Class Implementation
OptionScrollerMenuItem::OptionScrollerMenuItem(const std::string& label, std::vector<std::string> options, int& selectedIndex, std::function<void()> action, const std::string& description)
    : MenuItem(label, description), options(options), selectedIndex(selectedIndex), action(action) {
}

void OptionScrollerMenuItem::OnSelect(MenuManager& manager) {
    action();
    manager.TriggerActionFeedback();
}

void OptionScrollerMenuItem::OnSelectLeft() {
    if (selectedIndex > 0) {
        selectedIndex--;
    }
}

void OptionScrollerMenuItem::OnSelectRight() {
    if (selectedIndex < options.size() - 1) {
        selectedIndex++;
    }
}

void OptionScrollerMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;

    // Draw the label aligned to the left
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());

    // Draw the option text aligned to the right
    std::string optionText = options[selectedIndex];

    // Calculate position for right alignment with proper padding
    float textX = 110 + menuWidth - theme.rightPadding -
        DebugDrawCommon::GetTextWidthHeight(optionText.c_str(), theme.itemFontSize).x;

    int optionColor = BLUE_SELECTED; // Always use blue for options
    DebugDrawCommon::DrawTextPrintV(textX, y_position, theme.itemFontSize, optionColor, optionText.c_str());
}

bool OptionScrollerMenuItem::IsScrollable() {
    return true;
}

// BooleanMenuItem Class Implementations
BooleanMenuItem::BooleanMenuItem(const std::string& label, bool& value, std::function<void()> action, const std::string& description)
    : MenuItem(label, description), value(value), action(action) {
}

void BooleanMenuItem::OnSelect(MenuManager& manager) {
    value = !value; // Toggle the boolean value
    action();
    manager.TriggerActionFeedback();
}

void BooleanMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    // Only use selection color for the label, not blue
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;

    // Draw the label
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());

    // Draw the ON/OFF status aligned to the right
    //std::string statusText = value ? "ON" : "OFF";
    std::string statusText = value ? "\u030E" : "\u030F"; // tick, cross


    // Calculate position for right alignment with proper padding
    float textX = 110 + menuWidth - theme.rightPadding -
        DebugDrawCommon::GetTextWidthHeight(statusText.c_str(), theme.itemFontSize).x;

    // Only make "ON" blue, leave "OFF" with default color
    //int statusColor = value ? theme.statusOnColor : DEFAULT_COLOR;
    int statusColor = value ? _rgba(255, 255, 255, 255) : _rgba(224, 224, 224, 192); // tick, cross color

    DebugDrawCommon::DrawTextPrintV(textX, y_position, theme.itemFontSize, statusColor, statusText.c_str());
}

// ActionMenuItem Class Implementations
ActionMenuItem::ActionMenuItem(const std::string& label, std::function<void()> action, const std::string& description)
    : MenuItem(label, description), action(action) {
}

void ActionMenuItem::OnSelect(MenuManager& manager) {
    action();
    manager.TriggerActionFeedback();
}

void ActionMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());
}

// SubmenuMenuItem Class Implementations
SubmenuMenuItem::SubmenuMenuItem(const std::string& label, std::function<void(MenuManager&)> openSubmenu, const std::string& description)
    : MenuItem(label, description), openSubmenu(openSubmenu) {
}

void SubmenuMenuItem::OnSelect(MenuManager& manager) {
    openSubmenu(manager);
    manager.TriggerActionFeedback();
}

void SubmenuMenuItem::Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const {
    int color = isSelected ? SELECTED_COLOR : DEFAULT_COLOR;

    // Draw the label
    DebugDrawCommon::DrawTextPrintV(110 + theme.menuPadding, y_position, theme.itemFontSize, color, label.c_str());

    // Draw the ">" indicator aligned to the right
    std::string arrowText = ">";

    // Calculate position for right alignment with proper padding
    float textX = 110 + menuWidth - theme.rightPadding -
        DebugDrawCommon::GetTextWidthHeight(arrowText.c_str(), theme.itemFontSize).x;

    DebugDrawCommon::DrawTextPrintV(textX, y_position, theme.itemFontSize, color, arrowText.c_str());
}

// Menu Class Implementations
Menu::Menu(const std::string& title, int visibleItemsCount)
    : title(title), visibleItemsCount(visibleItemsCount), selectedItemIndex(0), scrollOffset(0), isWidthCalculated(false), menuWidth(0.0f) {
    // Use default of 16 if not specified
    if (this->visibleItemsCount == 0) {
        this->visibleItemsCount = 16;
    }
}

void Menu::AddMenuItem(std::unique_ptr<MenuItem> item) {
    items.push_back(std::move(item));
    isWidthCalculated = false; // Recalculate width when a new item is added
}

void Menu::SelectNext() {
    if (!items.empty()) {
        selectedItemIndex = (selectedItemIndex + 1) % items.size();
        AdjustScrollOffset();
    }
}

void Menu::SelectPrevious() {
    if (!items.empty()) {
        selectedItemIndex = (selectedItemIndex - 1 + items.size()) % items.size();
        AdjustScrollOffset();
    }
}

void Menu::HandleLeftInput() {
    if (items.empty()) return;

    isWidthCalculated = false;
    CalculateMenuWidth();

    MenuItem& currentItem = *items[selectedItemIndex];

    if (currentItem.IsScrollable()) {
        currentItem.OnSelectLeft();
    } else {
        // Not a scroller - jump 5 items backward
        selectedItemIndex = (selectedItemIndex - 5 + items.size()) % items.size();
        AdjustScrollOffset();
    }
}

void Menu::HandleRightInput() {
    if (items.empty()) return;

    isWidthCalculated = false;
    CalculateMenuWidth();

    MenuItem& currentItem = *items[selectedItemIndex];

    if (currentItem.IsScrollable()) {
        currentItem.OnSelectRight();
    }
    else {
        // Not a scroller - jump 5 items forward
        selectedItemIndex = (selectedItemIndex + 5) % items.size();
        AdjustScrollOffset();
    }
}

void Menu::HandleSelectInput() {
    if (!items.empty()) {
        // This will be called by MenuManager with the appropriate manager instance
    }
}

void Menu::RenderFooter(const std::string& description) {
    float startY = layout.startY +
        (customMin(static_cast<int>(items.size()), visibleItemsCount) * theme.rowHeight);

    // Calculate how tall the footer needs to be
    float maxDescWidth = menuWidth - theme.menuPadding - theme.rightPadding;
    float lineHeight = 15.0f;

    // Define padding values for the footer
    float footerTopPadding = 15.0f;
    float footerBottomPadding = 15.0f;

    std::vector<std::string> descLines;
    float footerHeight = theme.footerHeight; // Minimum height

    if (!description.empty()) {
        descLines = WrapText(description, maxDescWidth, theme.descriptionFontSize);

        // Calculate footer height based on text content plus top and bottom padding
        float contentHeight = descLines.size() * lineHeight;
        float minHeightWithPadding = contentHeight + footerTopPadding + footerBottomPadding;

        // Use the larger of minimum height and calculated height with padding
        footerHeight = customMax(footerHeight, minHeightWithPadding);
    }

    // Draw footer background (always present)
    DebugDrawCommon::DrawRectangle(
        layout.startX,
        startY + 1.0f, // Just below the divider
        layout.startX + menuWidth,
        startY + footerHeight,
        theme.footerBackgroundColor
    );

    // Draw description text if exists
    if (!description.empty()) {
        // Calculate vertical center of the footer for better positioning
        float totalTextHeight = descLines.size() * lineHeight;
        float verticalCenter = (footerHeight - totalTextHeight) / 2.0f;

        // Start drawing text from the vertical center position
        float lineY = startY + verticalCenter;

        for (const auto& line : descLines) {
            // Calculate center position for each line
            float lineWidth = CalculateTextWidth(line, theme.descriptionFontSize);
            float centerX = layout.startX + (menuWidth - lineWidth) / 2.0f;

            DebugDrawCommon::DrawTextPrintV(
                centerX,
                lineY,
                theme.descriptionFontSize,
                theme.descriptionColor,
                line.c_str()
            );
            lineY += lineHeight;
        }
    }
}

// Improved menu width calculation
void Menu::CalculateMenuWidth() {
    if (isWidthCalculated) return;  // Skip if width is already calculated

    menuWidth = 0.0f; // Reset menu width before recalculating

    // First pass: Calculate width needed for menu items
    for (const auto& item : items) {
        // Get width for the label
        float labelWidth = CalculateTextWidth(item->label, theme.itemFontSize);

        // Get width for values (approximate for different item types)
        float valueWidth = 0.0f;

        switch (item->GetKind()) {
        case MenuItem::Kind::IntScroller:
            valueWidth = CalculateTextWidth("9999", theme.itemFontSize); // Approximate for integer value
            break;
        case MenuItem::Kind::FloatScroller:
            valueWidth = CalculateTextWidth("100.00", theme.itemFontSize); // Approximate for float value
            break;
        case MenuItem::Kind::OptionScroller: {
            // Find longest option text
            float maxOptionWidth = 0.0f;
            for (const auto& option : static_cast<const OptionScrollerMenuItem*>(item.get())->options) {
                float optWidth = CalculateTextWidth(option, theme.itemFontSize);
                maxOptionWidth = customMax(maxOptionWidth, optWidth);
            }
            valueWidth = maxOptionWidth;
            break;
        }
        case MenuItem::Kind::Boolean:
            valueWidth = CalculateTextWidth("OFF", theme.itemFontSize); // OFF is wider than ON
            break;
        case MenuItem::Kind::Submenu:
            valueWidth = CalculateTextWidth(">", theme.itemFontSize);
            break;
        case MenuItem::Kind::Other:
            break;
        }

        // Total width needed for this item
        float itemWidth = labelWidth + valueWidth + theme.menuPadding + theme.rightPadding + 20.0f;
        menuWidth = customMax(menuWidth, itemWidth);
    }

    // Second pass: Consider description text width
    for (const auto& item : items) {
        if (!item->description.empty()) {
            // For descriptions, calculate full line width instead of just checking individual words
            // We'll try to fit the entire description without wrapping first
            float descWidth = CalculateTextWidth(item->description, theme.descriptionFontSize);

            // Add padding for the description
            descWidth += theme.menuPadding + theme.rightPadding;

            // Cap to a reasonable maximum width to avoid extremely wide menus
            descWidth = customMin(descWidth, theme.maxMenuWidth - theme.menuPadding - theme.rightPadding);

            menuWidth = customMax(menuWidth, descWidth);
        }
    }

    // Check title width
    float titleWidth = CalculateTextWidth(title, theme.titleFontSize) +
        theme.titlePadding + theme.rightPadding + 50.0f;

    menuWidth = customMax(menuWidth, titleWidth);

    // Ensure the menu doesn't get too wide
    menuWidth = customMin(menuWidth, theme.maxMenuWidth);

    isWidthCalculated = true;  // Mark width as calculated
}

// Complete Enhanced Menu Implementation Using Available Drawing Functions
void Menu::Render(MenuManager& manager) {
    // Calculate menu width always
    CalculateMenuWidth();

    // Determine how many items to display
    int actualVisibleItems = visibleItemsCount;

    // Cap to the actual number of items if less than visibleItemsCount
    actualVisibleItems = customMin(actualVisibleItems, static_cast<int>(items.size()));

    // Calculate total height needed for the menu
    float totalHeight = (actualVisibleItems * theme.rowHeight);

    // Calculate heights for various sections
    float titleBarHeight = theme.rowHeight * 1.5f; // Making title bar taller

    // Draw main menu background (excluding footer for now)
    DebugDrawCommon::DrawRectangle(
        layout.startX,
        layout.startY - titleBarHeight, // Include title bar
        layout.startX + menuWidth,
        layout.startY + totalHeight,
        theme.backgroundColor
    );

    // Draw title bar background
    DebugDrawCommon::DrawRectangle(
        layout.startX,
        layout.startY - titleBarHeight,
        layout.startX + menuWidth,
        layout.startY,
        theme.titleBackgroundColor
    );

    // Draw divider between items and footer (thin rectangle)
    DebugDrawCommon::DrawRectangle(
        layout.startX,
        layout.startY + totalHeight,
        layout.startX + menuWidth,
        layout.startY + totalHeight + 1.0f, // 1 pixel height
        theme.dividerColor
    );

    // Draw alternating row backgrounds
    for (int i = 0; i < actualVisibleItems; i++) {
        if (i % 2 == 1) { // Only draw for odd rows
            float topY = layout.startY + (i * theme.rowHeight);
            DebugDrawCommon::DrawRectangle(
                layout.startX,
                topY,
                layout.startX + menuWidth,
                topY + theme.rowHeight,
                theme.alternateRowColor
            );
        }
    }

    // Draw highlight for selected item
    {
        int displayIndex = selectedItemIndex - scrollOffset;
        if (displayIndex >= 0 && displayIndex < actualVisibleItems) {
            float topY = layout.startY + (displayIndex * theme.rowHeight);

            // Check if showing action feedback
            int highlightColor = theme.selectedHighlightColor;
            if (manager.IsActionFeedbackActive()) {
                highlightColor = FEEDBACK_FLASH_COLOR;
            }

            // Draw a subtle highlight behind the selected item
            DebugDrawCommon::DrawRectangle(
                layout.startX,
                topY,
                layout.startX + menuWidth,
                topY + theme.rowHeight,
                highlightColor
            );
        }
    }

    // Title text is properly vertical aligned and left-padded
    float titleY = layout.startY - titleBarHeight + (titleBarHeight - DebugDrawCommon::GetTextWidthHeight("M", theme.titleFontSize).y) / 2.0f;

    // Render the title in the title bar (left-aligned with title padding)
    DebugDrawCommon::DrawTextPrintV(
        layout.startX + theme.titlePadding,
        titleY,
        theme.titleFontSize,
        theme.titleColor,
        title.c_str()
    );

    // Draw a counter in the title bar (right-aligned)
    std::string counterText = std::to_string(selectedItemIndex + 1) + "/" + std::to_string(items.size());
    vec2 counterSize = DebugDrawCommon::GetTextWidthHeight(counterText.c_str(), theme.counterFontSize);

    // Use a smaller padding specifically for the counter
    float counterRightPadding = 15.0f; // Reduced from theme.rightPadding

    // For better vertical centering, calculate based on the actual counter text height
    float counterHeight = counterSize.y;
    float titleBarMiddle = layout.startY - (titleBarHeight / 2.0f);
    float counterY = titleBarMiddle - (counterHeight / 2.0f);

    // Draw counter text (right-aligned with less padding, properly centered)
    DebugDrawCommon::DrawTextPrintV(
        layout.startX + menuWidth - counterSize.x - counterRightPadding,
        counterY,
        theme.counterFontSize,
        theme.counterColor,
        counterText.c_str()
    );

    // Render scrollbar if needed
    if (layout.showScrollbar && items.size() > actualVisibleItems) {
        DrawScrollbar();
    }

    // Render each item in the menu
    int startIndex = customMax(0, scrollOffset);
    int endIndex = customMin(static_cast<int>(items.size()), scrollOffset + actualVisibleItems);
    for (int i = startIndex; i < endIndex; ++i) {
        int y_position = layout.startY + 6 + ((i - scrollOffset) * theme.rowHeight);
        items[i]->Render(y_position, i == selectedItemIndex, theme, menuWidth);
    }

    // Handle select input here since we have access to the manager
    if (DC::JOYPAD_COMMAND_ACTIVE(SID("menu-select")) && !items.empty()) {
        MenuItem& item = *items[selectedItemIndex];
        item.OnSelect(manager);
    }

    // Always render the footer area (with or without description text)
    if (!items.empty()) {
        RenderFooter(items[selectedItemIndex]->description);
    }
    else {
        // Render empty footer if there are no items
        RenderFooter("");
    }
}

// Enhanced scrollbar implementation
void Menu::DrawScrollbar() {
    // Only draw scrollbar if there are more items than can be displayed
    if (items.size() <= visibleItemsCount) return;

    int actualVisibleItems = customMin(visibleItemsCount, static_cast<int>(items.size()));
    float scrollbarHeight = actualVisibleItems * theme.rowHeight;
    float scrollbarX = layout.startX + menuWidth + 2.0f; // Position just to the right of the menu

    // Calculate scrollbar dimensions
    float scrollbarWidth = theme.scrollbarWidth;

    // Draw scrollbar background (track)
    DebugDrawCommon::DrawRectangle(
        scrollbarX,
        layout.startY,
        scrollbarX + scrollbarWidth,
        layout.startY + scrollbarHeight,
        theme.scrollbarBackgroundColor
    );

    // Calculate scrollbar thumb position and size
    float totalItems = static_cast<float>(items.size());
    float thumbHeight = (actualVisibleItems / totalItems) * scrollbarHeight;

    // Ensure minimum thumb size for better visibility
    thumbHeight = customMax(thumbHeight, 15.0f);

    // Calculate thumb position
    float scrollPercentage = static_cast<float>(scrollOffset) / customMax(totalItems - actualVisibleItems, 1.0f);
    float availableTrackHeight = scrollbarHeight - thumbHeight;
    float thumbY = layout.startY + (scrollPercentage * availableTrackHeight);

    // Draw scrollbar thumb
    DebugDrawCommon::DrawRectangle(
        scrollbarX,
        thumbY,
        scrollbarX + scrollbarWidth,
        thumbY + thumbHeight,
        theme.scrollbarColor
    );

    // Optional: Add thumb grip lines for a more detailed look
    if (scrollbarWidth >= 5.0f) { // Only if scrollbar is wide enough
        float lineColor = theme.backgroundColor; // Use background color for grip lines

        // Draw 3 horizontal grip lines in the middle of the thumb
        float middleY = thumbY + (thumbHeight / 2);
        float lineSpacing = 2.0f;

        for (int i = -1; i <= 1; i++) {
            float gripY = middleY + (i * lineSpacing);
            DebugDrawCommon::DrawRectangle(
                scrollbarX + 1.0f,
                gripY,
                scrollbarX + scrollbarWidth - 1.0f,
                gripY + 1.0f,
                lineColor
            );
        }
    }
}

void Menu::AdjustScrollOffset() {
    if (selectedItemIndex < scrollOffset) {
        scrollOffset = selectedItemIndex;
    }

    int actualVisibleItems = customMin(visibleItemsCount, static_cast<int>(items.size()));
    if (selectedItemIndex >= scrollOffset + actualVisibleItems) {
        scrollOffset = selectedItemIndex - actualVisibleItems + 1;
    }
}

float Menu::GetMenuWidth() {
    if (!isWidthCalculated) {
        CalculateMenuWidth();
    }
    return menuWidth;
}

const std::vector<std::unique_ptr<MenuItem>>& Menu::GetMenuItems() const { return items; }
int Menu::GetSelectedIndex() const { return selectedItemIndex; }
void Menu::SetSelectedIndex(int index) { selectedItemIndex = index; }
const std::string& Menu::GetTitle() const { return title; }
std::string Menu::GetSelectedItemName() const {
    if (selectedItemIndex >= 0 && selectedItemIndex < items.size()) {
        return items[selectedItemIndex]->GetName();
    }
    return "";
}

void Menu::RefreshMenu() {
    // Clear existing items
    ClearMenuItems();

    // Reset selection and scroll state
    selectedItemIndex = 0;
    scrollOffset = 0;

    // Force width recalculation
    isWidthCalculated = false;
    menuWidth = 0.0f;

    // Note: After calling RefreshMenu(), you need to call your
    // menu population function to re-add all the menu items
}

void Menu::ClearMenuItems() {
    // Clear the items vector
    // Note: If you're managing memory manually, you might need to delete the items first
    // Uncomment the following lines if you need to free memory:
    /*
    for (auto* item : items) {
        delete item;
    }
    */

    items.clear();

    // Reset width calculation flag
    isWidthCalculated = false;
}
