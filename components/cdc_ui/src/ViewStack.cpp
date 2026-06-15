/**
 * \file
 * \brief View-stack implementation with recursive mutex.
 *
 * The ViewStack is touched from multiple FreeRTOS contexts (UI task, USB
 * CTAP-HID task, BLE callback task) so the underlying storage needs a
 * synchronization primitive. The public API is reentrant: view callbacks
 * fired from inside dispatchKey/dispatchLongPress/dispatchTick legitimately
 * call back into push/pop/showModal/hideModal on the same task. A
 * FreeRTOS recursive mutex is therefore required - a plain mutex would
 * self-deadlock the UI task as soon as the first key is dispatched.
 */

#include "cdc_ui/ViewStack.h"
#include "cdc_core/EventBus.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <cstring>

static const char* TAG = "ViewStack";

namespace cdc::ui {

/**
 * \brief Checks whether a view is a `ListView` by runtime name.
 */
static bool isListView(const IView* view) {
    return view && (std::strcmp(view->getName(), "ListView") == 0);
}

/**
 * \brief Returns singleton view-stack instance.
 */
ViewStack& ViewStack::instance() {
    static ViewStack instance;
    instance.ensureMutex();
    return instance;
}

/**
 * \brief Lazily creates the FreeRTOS mutex on first access.
 */
void ViewStack::ensureMutex() {
    if (!mutex_) {
        mutex_ = xSemaphoreCreateRecursiveMutex();
    }
}

namespace {

/** \brief Scoped lock guard for the recursive ViewStack mutex. */
class StackLock {
public:
    explicit StackLock(SemaphoreHandle_t m) : m_(m) {
        if (m_) xSemaphoreTakeRecursive(m_, portMAX_DELAY);
    }
    ~StackLock() {
        if (m_) xSemaphoreGiveRecursive(m_);
    }
    StackLock(const StackLock&) = delete;
    StackLock& operator=(const StackLock&) = delete;
private:
    SemaphoreHandle_t m_;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Unlocked helpers. Caller MUST hold mutex_.
// ---------------------------------------------------------------------------

void ViewStack::push_unlocked(IView* view, void* context) {
    if (!view) {
        LOG_W(TAG, "Attempted to push null view");
        return;
    }
    if (exclusiveOwner_ && exclusiveOwner_ != view) {
        LOG_W(TAG, "push('%s') blocked: exclusive lock held by %p",
              view->getName(), exclusiveOwner_);
        return;
    }
    if (depth_ >= MAX_DEPTH) {
        LOG_E(TAG, "ViewStack overflow (max %d)", MAX_DEPTH);
        return;
    }

    if (depth_ > 0 && stack_[depth_ - 1]) {
        stack_[depth_ - 1]->onPause();  // counterpart to onResume() on pop
    }
    stack_[depth_++] = view;
    view->onEnter(context);
    needsFullRefresh_ = !(isListView(view) && isListView(depth_ > 1 ? stack_[depth_ - 2] : nullptr));

    LOG_D(TAG, "Pushed view '%s' (depth=%d)", view->getName(), depth_);
}

void ViewStack::pop_unlocked() {
    if (depth_ <= 1) {
        LOG_W(TAG, "Cannot pop root view");
        return;
    }

    IView* top = stack_[depth_ - 1];
    if (exclusiveOwner_ && exclusiveOwner_ != top) {
        LOG_W(TAG, "pop blocked: exclusive lock held by %p (top='%s')",
              exclusiveOwner_, top ? top->getName() : "(null)");
        return;
    }

    depth_--;
    if (top) {
        top->onExit();
        LOG_D(TAG, "Popped view '%s' (depth=%d)", top->getName(), depth_);
    }
    stack_[depth_] = nullptr;

    if (depth_ > 0 && stack_[depth_ - 1]) {
        stack_[depth_ - 1]->onResume();
    }
    needsFullRefresh_ = !(isListView(stack_[depth_ - 1]) && isListView(top));
}

void ViewStack::hideModal_unlocked() {
    if (modalDepth_ == 0) return;

    IView* top = modals_[--modalDepth_];
    modals_[modalDepth_] = nullptr;
    LOG_D(TAG, "Hiding modal '%s' (depth=%d)", top->getName(), modalDepth_);
    top->onExit();

    // The dismissed modal must be erased and whatever it covered repainted, so
    // force a full composite of the base view plus any modals still beneath.
    needsFullRefresh_ = true;
    if (modalDepth_ > 0) {
        modals_[modalDepth_ - 1]->markDirty();
    } else {
        IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
        if (view) {
            view->onResume();
            view->markDirty();
        }
    }
}

void ViewStack::removeModal_unlocked(IView* modal) {
    if (!modal) return;
    int idx = -1;
    for (uint8_t i = 0; i < modalDepth_; ++i) {
        if (modals_[i] == modal) { idx = i; break; }
    }
    if (idx < 0) return;

    modal->onExit();
    LOG_D(TAG, "Removing modal '%s' (depth=%d)", modal->getName(), modalDepth_ - 1);
    for (uint8_t i = static_cast<uint8_t>(idx); i + 1 < modalDepth_; ++i) {
        modals_[i] = modals_[i + 1];
    }
    modalDepth_--;
    modals_[modalDepth_] = nullptr;

    needsFullRefresh_ = true;
    if (modalDepth_ > 0) {
        modals_[modalDepth_ - 1]->markDirty();
    } else {
        IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
        if (view) {
            view->onResume();
            view->markDirty();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API. Each entry point acquires the mutex once.
// ---------------------------------------------------------------------------

void ViewStack::push(IView* view, void* context) {
    StackLock lock(mutex_);
    push_unlocked(view, context);
}

void ViewStack::pop() {
    StackLock lock(mutex_);
    pop_unlocked();
}

void ViewStack::replace(IView* view, void* context) {
    StackLock lock(mutex_);
    if (!view) {
        LOG_W(TAG, "Attempted to replace with null view");
        return;
    }
    if (depth_ == 0) {
        push_unlocked(view, context);
        return;
    }

    IView* top = stack_[depth_ - 1];
    if (exclusiveOwner_ && exclusiveOwner_ != view && exclusiveOwner_ != top) {
        LOG_W(TAG, "replace('%s') blocked: exclusive lock held by %p",
              view->getName(), exclusiveOwner_);
        return;
    }

    if (top) {
        top->onExit();
        LOG_D(TAG, "Replaced view '%s'", top->getName());
    }

    stack_[depth_ - 1] = view;
    view->onEnter(context);
    needsFullRefresh_ = !(isListView(view) && isListView(top));

    LOG_D(TAG, "Replaced with view '%s'", view->getName());
}

void ViewStack::popToRoot() {
    StackLock lock(mutex_);
    while (depth_ > 1) {
        pop_unlocked();
    }
}

void ViewStack::popToAnchor(IView* anchor) {
    StackLock lock(mutex_);
    while (depth_ > 1) {
        IView* cur = stack_[depth_ - 1];
        if (cur == anchor) break;
        pop_unlocked();
    }
}

void ViewStack::popToDepth(uint8_t targetDepth) {
    StackLock lock(mutex_);
    while (depth_ > targetDepth && depth_ > 1) {
        pop_unlocked();
    }
}

IView* ViewStack::current() const {
    StackLock lock(mutex_);
    if (depth_ == 0) return nullptr;
    return stack_[depth_ - 1];
}

IView* ViewStack::at(uint8_t idx) const {
    StackLock lock(mutex_);
    if (idx >= depth_) return nullptr;
    return stack_[idx];
}

void ViewStack::dispatchKey(char key) {
    cdc::core::EventBus::instance().publish(cdc::core::EventType::KEY_PRESSED,
                                            static_cast<uint8_t>(key));
    StackLock lock(mutex_);
    resetInactivityTimer();

    if (modalDepth_ > 0) {
        // Input stays on the top modal and never falls through to the view (or
        // lower modals) behind it.
        InputResult result = modals_[modalDepth_ - 1]->onKey(key);
        if (result == InputResult::REQUEST_POP) {
            hideModal_unlocked();
        }
        return;
    }

    IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
    if (view) {
        InputResult result = view->onKey(key);
        if (result == InputResult::REQUEST_POP) {
            pop_unlocked();
        }
    }
}

void ViewStack::dispatchLongPress(char key) {
    cdc::core::EventBus::instance().publish(cdc::core::EventType::KEY_LONG_PRESS,
                                            static_cast<uint8_t>(key));
    StackLock lock(mutex_);

    if (modalDepth_ > 0) {
        InputResult result = modals_[modalDepth_ - 1]->onLongPress(key);
        // 'N' is the universal back/cancel gesture: hide the top modal unless it
        // consumed the press itself. Other keys hide only on REQUEST_POP.
        if (result == InputResult::REQUEST_POP ||
            (key == 'N' && result != InputResult::CONSUMED)) {
            hideModal_unlocked();
        }
        return;
    }

    IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
    if (!view) {
        return;
    }
    InputResult result = view->onLongPress(key);
    // 'N' is the universal back/cancel gesture: pop unless the view consumed it
    // (e.g. an input view that cancels itself and notifies its owner). Other
    // keys pop only on an explicit REQUEST_POP.
    if (result == InputResult::REQUEST_POP ||
        (key == 'N' && result != InputResult::CONSUMED && depth_ > 1)) {
        pop_unlocked();
    }
}

void ViewStack::dispatchTick(uint32_t nowMs) {
    StackLock lock(mutex_);
    if (modalDepth_ > 0) {
        modals_[modalDepth_ - 1]->onTick(nowMs);
    }
    IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
    if (view) {
        view->onTick(nowMs);
    }
}

void ViewStack::render(bool synchronous) {
    StackLock lock(mutex_);
    IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
    if (!view) {
        return;
    }

    hal::IDisplay* display = hal::getDisplayInstance();

    if (modalDepth_ > 0) {
        // Modals own the screen and stack on top of the base view. Repaint the
        // base first (when dirty or on a forced full refresh, e.g. after a modal
        // was dismissed) and then draw every modal bottom-to-top in the same
        // pass, so the composite stays correct and nothing of a gone modal lingers.
        bool baseDirty = view->needsRender();
        bool anyModalDirty = false;
        for (uint8_t i = 0; i < modalDepth_; ++i) {
            if (modals_[i]->needsRender()) { anyModalDirty = true; break; }
        }
        if (!baseDirty && !anyModalDirty && !needsFullRefresh_) {
            return;
        }
        if (baseDirty || needsFullRefresh_) {
            view->render(false);
            view->clearDirty();
        }
        for (uint8_t i = 0; i < modalDepth_; ++i) {
            modals_[i]->render(true);
            modals_[i]->clearDirty();
        }
        if (display) {
            // A dirty base under a still-visible modal repaints the composite in
            // the framebuffer; PARTIAL suffices. FULL is reserved for modal
            // stack changes (needsFullRefresh_) to erase a dismissed modal.
            hal::RefreshMode mode = needsFullRefresh_
                ? hal::RefreshMode::FULL : hal::RefreshMode::PARTIAL;
            if (synchronous) display->flushSync(mode);
            else             display->flush(mode);
        }
        needsFullRefresh_ = false;
        return;
    }

    if (!view->needsRender()) {
        return;
    }
    view->render(false);
    view->clearDirty();

    hal::RefreshMode mode = needsFullRefresh_
        ? hal::RefreshMode::FULL
        : (view->prefersLightRefresh() ? hal::RefreshMode::PARTIAL_LIGHT : hal::RefreshMode::PARTIAL);
    if (display) {
        if (synchronous) display->flushSync(mode);
        else             display->flush(mode);
    }
    needsFullRefresh_ = false;
}

bool ViewStack::needsRender() const {
    StackLock lock(mutex_);
    for (uint8_t i = 0; i < modalDepth_; ++i) {
        if (modals_[i]->needsRender()) return true;
    }
    IView* view = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
    return view && view->needsRender();
}

void ViewStack::showModal(IView* modal) {
    StackLock lock(mutex_);
    if (!modal) return;
    if (exclusiveOwner_) {
        LOG_W(TAG, "showModal('%s') blocked: exclusive lock held by %p",
              modal->getName(), exclusiveOwner_);
        return;
    }

    // If this modal is already stacked, lift it back to the top rather than
    // duplicating it (the shared toast/confirm singletons get re-shown in place).
    for (uint8_t i = 0; i < modalDepth_; ++i) {
        if (modals_[i] == modal) {
            for (uint8_t j = i + 1; j < modalDepth_; ++j) modals_[j - 1] = modals_[j];
            modalDepth_--;
            break;
        }
    }

    // Stack full: drop the oldest modal at the bottom to make room.
    if (modalDepth_ >= MAX_MODAL_DEPTH) {
        modals_[0]->onExit();
        for (uint8_t i = 1; i < modalDepth_; ++i) modals_[i - 1] = modals_[i];
        modalDepth_--;
    }

    // Stacking on top of an existing modal needs a full composite so a smaller
    // new modal does not leave the previous one's edges showing around it. The
    // first modal over the base view stays a partial (no toast-refresh regress).
    if (modalDepth_ > 0) needsFullRefresh_ = true;

    if (modalDepth_ > 0) {
        modals_[modalDepth_ - 1]->onPause();
    } else {
        IView* base = (depth_ == 0) ? nullptr : stack_[depth_ - 1];
        if (base) base->onPause();  // counterpart to onResume() in hideModal
    }
    modals_[modalDepth_++] = modal;
    modal->onEnter(nullptr);
    LOG_D(TAG, "Showing modal '%s' (depth=%d)", modal->getName(), modalDepth_);
}

void ViewStack::hideModal() {
    StackLock lock(mutex_);
    hideModal_unlocked();
}

void ViewStack::removeModal(IView* modal) {
    StackLock lock(mutex_);
    removeModal_unlocked(modal);
}

void ViewStack::setInactivityTimeout(InactivityCallback callback, uint32_t timeoutMs) {
    StackLock lock(mutex_);
    inactivityCallback_ = callback;
    inactivityTimeoutMs_ = timeoutMs;
    lastActivityMs_ = 0;
    LOG_D(TAG, "Inactivity timeout set: %lu ms", timeoutMs);
}

void ViewStack::resetInactivityTimer() {
    StackLock lock(mutex_);
    lastActivityMs_ = 0;
}

void ViewStack::checkInactivity(uint32_t nowMs) {
    InactivityCallback cb = nullptr;
    bool triggered = false;
    uint32_t elapsedForLog = 0;

    {
        StackLock lock(mutex_);
        if (inactivityTimeoutMs_ == 0 || !inactivityCallback_) {
            return;
        }
        if (lastActivityMs_ == 0) {
            lastActivityMs_ = nowMs;
            return;
        }
        uint32_t elapsed = nowMs - lastActivityMs_;
        if (elapsed >= inactivityTimeoutMs_) {
            cb = inactivityCallback_;
            triggered = true;
            elapsedForLog = elapsed;
            lastActivityMs_ = nowMs;
        }
    }

    if (cb && triggered) {
        LOG_I(TAG, "Inactivity timeout triggered after %lu ms", elapsedForLog);
        cb();
    }
}

bool ViewStack::acquireExclusive(const void* owner) {
    StackLock lock(mutex_);
    if (!owner) {
        LOG_W(TAG, "acquireExclusive called with null owner");
        return false;
    }
    if (exclusiveOwner_ && exclusiveOwner_ != owner) {
        LOG_W(TAG, "Exclusive lock already held by %p, refusing %p", exclusiveOwner_, owner);
        return false;
    }
    exclusiveOwner_ = owner;
    LOG_D(TAG, "Exclusive lock acquired by %p", owner);
    return true;
}

bool ViewStack::releaseExclusive(const void* owner) {
    StackLock lock(mutex_);
    if (!exclusiveOwner_) {
        return false;
    }
    if (exclusiveOwner_ != owner) {
        LOG_W(TAG, "releaseExclusive: owner mismatch (held=%p, caller=%p)",
              exclusiveOwner_, owner);
        return false;
    }
    LOG_D(TAG, "Exclusive lock released by %p", owner);
    exclusiveOwner_ = nullptr;
    return true;
}

} // namespace cdc::ui
