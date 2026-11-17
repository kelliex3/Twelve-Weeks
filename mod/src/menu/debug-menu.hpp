#pragma once
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include "sid/sid.hpp"

// Forward declarations
class MenuManager;
class Menu;
class MenuItem;

// MenuManager class to handle menu state without conflicts
class MenuManager {
private:
    std::vector<std::unique_ptr<Menu>> menuStack;
    std::unique_ptr<Menu> currentMenu;
    bool menuActionFeedback;
    int menuActionFeedbackTimer;

public:
    MenuManager();

    // Menu navigation
    void PushMenu(std::unique_ptr<Menu> menu);
    bool PopMenu();
    bool IsEmpty();
    bool IsMenuInStack(const Menu& menu) const;
    void SetCurrentMenu(std::unique_ptr<Menu> menu);

    // Action feedback
    void TriggerActionFeedback();
    bool IsActionFeedbackActive() const;
    void UpdateActionFeedback();

    // Input handling
    void HandleInput();
    void Render();

    std::string GetSelectedItemName() const;
    MenuItem* GetSelectedMenuItem() const;
};

// Theme structure for menu appearance
struct MenuTheme {
    // Colors
    int backgroundColor;
    int alternateRowColor;
    int titleColor;
    int counterColor;
    int descriptionColor;
    int scrollbarColor;
    int scrollbarBackgroundColor;
    int titleBackgroundColor;
    int counterBackgroundColor;
    int dividerColor;
    int selectedHighlightColor;
    int statusOnColor;
    int footerBackgroundColor;

    // Fonts and sizes
    float titleFontSize;
    float itemFontSize;
    float descriptionFontSize;
    float counterFontSize;

    // Layout
    float menuPadding;
    float rightPadding;
    float rowHeight;
    float scrollbarWidth;
    float titlePadding;
    float footerHeight;
    float maxMenuWidth;

    // Constructor with default values
    MenuTheme()
        : backgroundColor(_rgba(0, 0, 0, 200)),
        alternateRowColor(_rgba(0, 0, 0, 150)),
        titleColor(_rgba(255, 255, 255, 255)),
        counterColor(_rgba(255, 255, 255, 255)),
        descriptionColor(_rgba(0, 208, 255, 255)),        // Blue for descriptions
        scrollbarColor(_rgba(255, 255, 255, 180)),
        scrollbarBackgroundColor(_rgba(0, 0, 0, 100)),
        titleBackgroundColor(_rgba(0, 0, 0, 230)),
        counterBackgroundColor(_rgba(0, 0, 0, 230)),
        dividerColor(_rgba(255, 255, 255, 70)),
        selectedHighlightColor(_rgba(255, 255, 255, 30)),
        statusOnColor(_rgba(0, 208, 255, 255)),           // Blue for "ON" status
        footerBackgroundColor(_rgba(0, 0, 0, 230)),       // Footer background
        titleFontSize(1.1f),
        itemFontSize(1.0f),
        descriptionFontSize(1.0f),
        counterFontSize(1.0f),
        menuPadding(15.0f),                               // Left padding
        rightPadding(60.0f),                              // Right padding for values
        rowHeight(28.0f),
        scrollbarWidth(5.0f),
        titlePadding(15.0f),                              // Title padding from left
        footerHeight(50.0f),                              // Minimum footer height
        maxMenuWidth(600.0f) {                            // Maximum menu width
    }
};

// Layout structure for menu positioning
struct MenuLayout {
    float startX;
    float startY;
    float titleOffsetY;
    float counterOffsetY;
    float descriptionOffsetY;
    bool showScrollbar;

    // Constructor with default values
    MenuLayout()
        : startX(80.0f),
        startY(100.0f),
        titleOffsetY(-25.0f),
        counterOffsetY(3.0f),
        descriptionOffsetY(23.0f),
        showScrollbar(true) {
    }
};

// Base MenuItem class
class MenuItem {
public:
    std::string label;
    std::string description;

    enum class Kind { Other, FloatScroller, IntScroller, OptionScroller, Boolean, Submenu };

    MenuItem(const std::string& label, const std::string& description = "");
    virtual ~MenuItem() = default;

    virtual Kind GetKind() const { return Kind::Other; }
    virtual void OnSelect(MenuManager& manager) = 0;
    virtual void OnSelectLeft() {}
    virtual void OnSelectRight() {}
    virtual void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const = 0;
    virtual bool IsScrollable() { return false; }
    const std::string& GetName() const { return label; }
protected:
    float GetRightAlignedX(const std::string& text, float menuWidth, float fontSize, float padding) const;
};

// Float scroller menu item
class FloatScrollerMenuItem : public MenuItem {
private:
    float& value;
    float minValue;
    float maxValue;
    float step;
    std::function<void()> action;

public:
    FloatScrollerMenuItem(const std::string& label, float& value, float minValue, float maxValue,
        float step, std::function<void()> action = []() {}, const std::string& description = "");

    Kind GetKind() const override { return Kind::FloatScroller; }
    void OnSelect(MenuManager& manager) override;
    void OnSelectLeft() override;
    void OnSelectRight() override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
    virtual bool IsScrollable() override;
};

// Integer scroller menu item
class IntScrollerMenuItem : public MenuItem {
private:
    int& value;
    int minValue;
    int maxValue;
    int step;
    std::function<void()> action;

public:
    IntScrollerMenuItem(const std::string& label, int& value, int minValue, int maxValue,
        int step, std::function<void()> action = []() {}, const std::string& description = "");

    Kind GetKind() const override { return Kind::IntScroller; }
    void OnSelect(MenuManager& manager) override;
    void OnSelectLeft() override;
    void OnSelectRight() override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
    bool IsScrollable() override;
};

// Option scroller menu item
class OptionScrollerMenuItem : public MenuItem {
private:
    std::vector<std::string> options;
    int& selectedIndex;
    std::function<void()> action;

public:
    OptionScrollerMenuItem(const std::string& label, std::vector<std::string> options, int& selectedIndex,
        std::function<void()> action = []() {}, const std::string& description = "");

    Kind GetKind() const override { return Kind::OptionScroller; }
    void OnSelect(MenuManager& manager) override;
    void OnSelectLeft() override;
    void OnSelectRight() override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
    bool IsScrollable() override;


    friend class Menu; // Allow Menu to access options for width calculation
};

// Boolean menu item
class BooleanMenuItem : public MenuItem {
private:
    bool& value;
    std::function<void()> action;

public:
    BooleanMenuItem(const std::string& label, bool& value, std::function<void()> action = []() {},
        const std::string& description = "");

    Kind GetKind() const override { return Kind::Boolean; }
    void OnSelect(MenuManager& manager) override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
};

// Action menu item
class ActionMenuItem : public MenuItem {
private:
    std::function<void()> action;

public:
    ActionMenuItem(const std::string& label, std::function<void()> action, const std::string& description = "");

    void OnSelect(MenuManager& manager) override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
};

// Submenu menu item
class SubmenuMenuItem : public MenuItem {
private:
    std::function<void(MenuManager&)> openSubmenu;

public:
    SubmenuMenuItem(const std::string& label, std::function<void(MenuManager&)> openSubmenu,
        const std::string& description = "");

    Kind GetKind() const override { return Kind::Submenu; }
    void OnSelect(MenuManager& manager) override;
    void Render(int y_position, bool isSelected, const MenuTheme& theme, float menuWidth) const override;
};

// Main Menu class
class Menu {
private:
    std::string title;
    std::vector<std::unique_ptr<MenuItem>> items;
    int selectedItemIndex;
    int scrollOffset;
    int visibleItemsCount;
    bool isWidthCalculated;
    float menuWidth;

    void CalculateMenuWidth();
    void AdjustScrollOffset();
    void DrawScrollbar();
    void RenderFooter(const std::string& description);
    float CalculateTextWidth(const std::string& text, float fontSize) const;

public:
    MenuTheme theme;
    MenuLayout layout;

    Menu(const std::string& title, int visibleItemsCount = 16);
    ~Menu() = default;

    void AddMenuItem(std::unique_ptr<MenuItem> item);
    void SelectNext();
    void SelectPrevious();
    void HandleLeftInput();
    void HandleRightInput();
    void HandleSelectInput();
    void Render(MenuManager& manager);

    float GetMenuWidth();

    const std::vector<std::unique_ptr<MenuItem>>& GetMenuItems() const;
    int GetSelectedIndex() const;
    void SetSelectedIndex(int index);
    const std::string& GetTitle() const;
    std::string GetSelectedItemName() const;

    void RefreshMenu();
    void ClearMenuItems();

    // Allow menu items to access certain private members
    friend class FloatScrollerMenuItem;
    friend class IntScrollerMenuItem;
    friend class OptionScrollerMenuItem;
    friend class BooleanMenuItem;
    friend class SubmenuMenuItem;
};

// Utility functions
std::vector<std::string> WrapText(const std::string& text, float maxWidth, float fontSize);
