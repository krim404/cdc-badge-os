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
    static constexpr uint8_t MAX_DEPTH = 20;

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
     * \brief Pops views until the stack depth is at most targetDepth.
     * \param targetDepth Depth to collapse down to; never pops below the root.
     *
     * Used by host_ui_pop_to_plugin to return to a plugin's first view in one
     * step, since plugin root views have no stable pointer for popToAnchor.
     */
    void popToDepth(uint8_t targetDepth);

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
     * @return Result reported by the receiving view (IGNORED if no view
     *         handled the press), so callers can wire global fallbacks.
     */
    InputResult dispatchLongPress(char key);

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
     * Show a modal overlay (e.g., toast, context menu). Modals stack: the new
     * one is pushed on top of any already-shown modal and receives input
     * exclusively. If the same modal is already stacked it is lifted to the top.
     * @param modal Modal view
     */
    void showModal(IView* modal);

    /**
     * Hide the top modal, revealing the next modal beneath it (or the base view).
     */
    void hideModal();

    /**
     * \brief Remove a specific modal from any position in the modal stack.
     *
     * Unlike hideModal() this does not require the modal to be on top; modals
     * stacked above it stay in place. Used to retract an overlay whose backing
     * view object is about to be destroyed (e.g. plugin teardown), so the stack
     * keeps no dangling pointer. No-op if the modal is not currently shown.
     * \param modal The modal view to remove.
     */
    void removeModal(IView* modal);

    /**
     * Check if any modal is active
     */
    bool hasModal() const { return modalDepth_ > 0; }

    /**
     * Get the top (input-receiving) modal view, or nullptr if none.
     */
    IView* getModal() const { return modalDepth_ > 0 ? modals_[modalDepth_ - 1] : nullptr; }

    /**
     * Force the next render to use FULL refresh (manual anti-ghosting).
     */
    void forceFullRefresh() { forceRefresh(hal::RefreshMode::FULL); }

    /**
     * \brief Escalates the refresh mode of the next render.
     * \param mode Minimum refresh mode to use; a stronger already-pending mode
     *        wins. FULL/FAST reset the HAL ghost-escalation counters as defined
     *        in EpaperDisplay. Any view may call this at any time (e.g. games
     *        or other high-churn content that wants a clean panel).
     *
     * Also marks the current view dirty so the request takes effect on the
     * next render pass without requiring a separate markDirty().
     */
    void forceRefresh(hal::RefreshMode mode);

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

    static constexpr uint8_t MAX_MODAL_DEPTH = 4;

    IView* stack_[MAX_DEPTH] = {};
    uint8_t depth_ = 0;
    IView* modals_[MAX_MODAL_DEPTH] = {};
    uint8_t modalDepth_ = 0;
    IView* pendingPush_ = nullptr;
    void* pendingContext_ = nullptr;
    // Strongest refresh mode requested for the next flush. Starts at FULL so
    // the very first render after boot fully cleans the panel; afterwards it
    // resets to PARTIAL_LIGHT ("no extra requirement") and transitions or
    // views escalate it as needed.
    hal::RefreshMode pendingRefresh_ = hal::RefreshMode::FULL;
    // The framebuffer composite (base view + modal stack) must be repainted
    // from the bottom up, e.g. after a modal was dismissed so nothing of it
    // lingers. Independent of the refresh waveform above.
    bool needsCompositeRepaint_ = false;
    const void* exclusiveOwner_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;

    void ensureMutex();

    // Unlocked helpers used when the caller already holds mutex_.
    void push_unlocked(IView* view, void* context);
    void pop_unlocked();
    void hideModal_unlocked();
    void removeModal_unlocked(IView* modal);
    void escalatePending_unlocked(hal::RefreshMode mode);

    // Inactivity timeout
    InactivityCallback inactivityCallback_ = nullptr;
    uint32_t inactivityTimeoutMs_ = 0;
    uint32_t lastActivityMs_ = 0;
};

} // namespace cdc::ui
