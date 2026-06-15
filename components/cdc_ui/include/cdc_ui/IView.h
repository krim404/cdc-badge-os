#pragma once

#include <cstdint>

namespace cdc::ui {

/**
 * Result of input handling
 */
enum class InputResult : uint8_t {
    CONSUMED,       // Input was handled, may need re-render
    IGNORED,        // Input was not handled
    REQUEST_POP,    // View wants to be popped (back/cancel)
    REQUEST_PUSH    // View wants to push a child view
};

/**
 * View interface - base for all UI screens
 *
 * Views are stateful screen components that handle rendering and input.
 * They are managed by ViewStack for navigation.
 *
 * Reference: ~/GIT/cdc-badge-os-legacy/components/cdc_badge/views.h
 */
class IView {
public:
    virtual ~IView() = default;

    // === Lifecycle ===

    /**
     * Called when view becomes active (pushed or becomes top)
     * @param context Optional context data from parent
     */
    virtual void onEnter(void* context = nullptr) = 0;

    /**
     * Called when view is being removed from stack
     */
    virtual void onExit() = 0;

    /**
     * Called when view becomes visible again (child popped)
     */
    virtual void onResume() = 0;

    /**
     * Called when the view is covered by another view or modal (still on the
     * stack but no longer active). Counterpart to onResume(). Default no-op.
     */
    virtual void onPause() {}

    // === Rendering ===

    /**
     * Render view content to display buffer
     * @param partial True for partial update, false for full refresh
     */
    virtual void render(bool partial) = 0;

    /**
     * Check if view needs re-rendering
     */
    virtual bool needsRender() const = 0;

    /**
     * Mark view as needing re-render
     */
    virtual void markDirty() = 0;

    /**
     * Clear the dirty flag. Called automatically by ViewStack after a
     * successful `render()` pass. Subclasses that animate or update every
     * tick must call `markDirty()` themselves in `onTick()`.
     */
    virtual void clearDirty() = 0;

    /**
     * Whether this view's partial updates should use RefreshMode::PARTIAL_LIGHT,
     * i.e. never be periodically promoted to a full refresh. Views whose updates
     * are tiny and low-churn (e.g. the lock-screen clock) return true so the
     * panel does not flicker through a forced full refresh while idle. A full
     * refresh still happens on the next view change.
     */
    virtual bool prefersLightRefresh() const { return false; }

    // === Input ===

    /**
     * Handle key press
     * @param key Key character ('0'-'9', 'Y', 'N', etc.)
     * @return Input result
     */
    virtual InputResult onKey(char key) = 0;

    /**
     * Handle long key press (optional)
     * @param key Key character
     * @return Input result
     */
    virtual InputResult onLongPress(char key) { (void)key; return InputResult::IGNORED; }

    // === Periodic ===

    /**
     * Called periodically for animations/timers
     * @param nowMs Current time in milliseconds
     */
    virtual void onTick(uint32_t nowMs) { (void)nowMs; }

    // === Footer ===

    /**
     * Get footer hint text (e.g., "[Y] OK [N] Back")
     * Return nullptr for no footer
     */
    virtual const char* getFooterHint() const { return nullptr; }

    /**
     * Override the footer hint at runtime. Default is a no-op; views that
     * support custom footers (ListView, T9InputView, ...) override this to
     * persist the hint. The string is NOT copied by the view - caller
     * (typically PluginUiState) must keep it alive while the view is shown.
     * \param hint Footer text, or nullptr to clear the override.
     */
    virtual void setFooterHint(const char* hint) { (void)hint; }

    /**
     * Register opaque hooks fired when this view is hidden (onPause) and shown
     * again (onResume). Default is a no-op; ViewBase persists and fires them.
     * Lets callers (e.g. PluginUiState) react to a view being covered/revealed
     * without enumerating concrete view types. Pass nullptr to clear a hook.
     * \param onHide Called from onPause(), or nullptr.
     * \param onShow Called from onResume(), or nullptr.
     * \param userData Opaque pointer passed back to both hooks.
     */
    virtual void setLifecycleHooks(void (*onHide)(void*), void (*onShow)(void*), void* userData) {
        (void)onHide; (void)onShow; (void)userData;
    }

    // === Identity ===

    /**
     * Get view name for debugging
     */
    virtual const char* getName() const = 0;
};

/**
 * ViewBase - Default implementation of IView
 *
 * Provides common functionality for views:
 * - Dirty flag management
 * - Title storage
 * - Default lifecycle handlers
 */
class ViewBase : public IView {
public:
    virtual ~ViewBase() = default;

    // Lifecycle with default implementations
    void onEnter(void* context) override {
        (void)context;
        dirty_ = true;
    }

    void onExit() override { }

    void onResume() override {
        dirty_ = true;
        if (onShow_) onShow_(lifecycleUserData_);
    }

    void onPause() override {
        if (onHide_) onHide_(lifecycleUserData_);
    }

    void setLifecycleHooks(void (*onHide)(void*), void (*onShow)(void*), void* userData) override {
        onHide_ = onHide;
        onShow_ = onShow;
        lifecycleUserData_ = userData;
    }

    // Dirty flag management
    bool needsRender() const override { return dirty_; }
    void markDirty() override { dirty_ = true; }
    void clearDirty() override { dirty_ = false; }

    // Footer override (universal). Subclasses that wire their own
    // getFooterHint() can still consult customFooter_ first.
    void setFooterHint(const char* hint) override {
        customFooter_ = hint;
        dirty_ = true;
    }
    const char* getFooterHint() const override { return customFooter_; }

protected:
    void setTitle(const char* title) { title_ = title; }
    const char* getTitle() const { return title_; }

    bool dirty_ = true;
    const char* title_ = nullptr;
    const char* customFooter_ = nullptr;

    void (*onHide_)(void*) = nullptr;
    void (*onShow_)(void*) = nullptr;
    void* lifecycleUserData_ = nullptr;
};

} // namespace cdc::ui
