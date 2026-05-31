#pragma once

#include "IView.h"
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace cdc::ui {

/**
 * ViewStack - Navigation stack for views
 *
 * Manages a stack of views for hierarchical navigation.
 * Supports push/pop/replace operations and modal overlays.
 *
 * Reference: ~/GIT/cdc-badge-os-legacy/main/app_input.cpp (state machine)
 */
class ViewStack {
public:
    static constexpr uint8_t MAX_DEPTH = 8;

    /**
     * Get singleton instance
     */
    static ViewStack& instance();

    /**
     * Push a view onto the stack
     * @param view View to push
     * @param context Optional context to pass to onEnter
     */
    void push(IView* view, void* context = nullptr);

    /**
     * Pop the top view from stack
     * Does nothing if only one view remains
     */
    void pop();

    /**
     * Replace top view with another
     * @param view New view
     * @param context Optional context
     */
    void replace(IView* view, void* context = nullptr);

    /**
     * Pop all views except root
     */
    void popToRoot();

    /**
     * \brief Pops views until the specified anchor view is the current view.
     * \param anchor Anchor view to return to.
     *
     * Stops popping if the anchor is reached or only the root view remains.
     * Useful for returning to a list view after wizard completion.
     */
    void popToAnchor(IView* anchor);

    /**
     * Get current (top) view
     */
    IView* current() const;

    /**
     * Get view at specific depth (0 = root)
     */
    IView* at(uint8_t depth) const;

    /**
     * Get current stack depth
     */
    uint8_t depth() const { return depth_; }

    /**
     * Check if stack is empty
     */
    bool isEmpty() const { return depth_ == 0; }

    // === Input dispatch ===

    /**
     * Dispatch key press to current view
     * @param key Key character
     */
    void dispatchKey(char key);

    /**
     * Dispatch long press to current view
     * @param key Key character
     */
    void dispatchLongPress(char key);

    /**
     * Dispatch tick to current view and modal
     * @param nowMs Current time
     */
    void dispatchTick(uint32_t nowMs);

    // === Rendering ===

    /**
     * \brief Render current view (and modal if present) and flush to display.
     * \param synchronous When true, flush via the blocking display path
     *        (flushSync) so the panel update has fully completed on return.
     *        When false (default), flush asynchronously via the render task.
     */
    void render(bool synchronous = false);

    /**
     * Check if any view needs rendering
     */
    bool needsRender() const;

    // === Modal support ===

    /**
     * Show a modal overlay (e.g., toast, context menu)
     * @param modal Modal view
     */
    void showModal(IView* modal);

    /**
     * Hide current modal
     */
    void hideModal();

    /**
     * Check if modal is active
     */
    bool hasModal() const { return modal_ != nullptr; }

    /**
     * Get modal view
     */
    IView* getModal() const { return modal_; }

    /**
     * Force next render to use FULL refresh
     * (called automatically after view changes)
     */
    void forceFullRefresh() { needsFullRefresh_ = true; }

    // === Exclusive lock (e.g. for FIDO2 prompts) ===

    /**
     * \brief Acquires exclusive ownership of the view stack.
     * \param owner Caller-supplied identity token (use the view pointer or a static address).
     * \return true if lock acquired, false if already held by someone else.
     *
     * While exclusive ownership is held, push/pop/replace/showModal from anyone
     * other than the owner are rejected with a warning log.
     */
    bool acquireExclusive(const void* owner);

    /**
     * \brief Releases exclusive ownership.
     * \param owner Must match the token used in acquireExclusive.
     * \return true if released, false if owner mismatch or not held.
     */
    bool releaseExclusive(const void* owner);

    /**
     * \brief Returns current exclusive owner, or nullptr if none.
     */
    const void* exclusiveOwner() const { return exclusiveOwner_; }

    // === Inactivity timeout ===

    /**
     * Callback type for inactivity timeout
     */
    using InactivityCallback = void(*)();

    /**
     * Set inactivity timeout callback
     * @param callback Function to call when timeout expires
     * @param timeoutMs Timeout in milliseconds (0 to disable)
     */
    void setInactivityTimeout(InactivityCallback callback, uint32_t timeoutMs);

    /**
     * Reset inactivity timer (called automatically on key press)
     */
    void resetInactivityTimer();

    /**
     * Check and handle inactivity (call in tick/loop)
     * @param nowMs Current time in milliseconds
     */
    void checkInactivity(uint32_t nowMs);

private:
    ViewStack() = default;

    IView* stack_[MAX_DEPTH] = {};
    uint8_t depth_ = 0;
    IView* modal_ = nullptr;
    IView* pendingPush_ = nullptr;
    void* pendingContext_ = nullptr;
    bool needsFullRefresh_ = true;  // True after view changes
    const void* exclusiveOwner_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;

    void ensureMutex();

    // Unlocked helpers used when the caller already holds mutex_.
    void push_unlocked(IView* view, void* context);
    void pop_unlocked();
    void hideModal_unlocked();

    // Inactivity timeout
    InactivityCallback inactivityCallback_ = nullptr;
    uint32_t inactivityTimeoutMs_ = 0;
    uint32_t lastActivityMs_ = 0;
};

} // namespace cdc::ui
