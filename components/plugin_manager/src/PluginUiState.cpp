#include "plugin_manager/PluginUiState.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_ui/ViewStack.h"
#include "host_str_conv.h"

#include <cstring>
#include <string>

extern "C" void* plg_get_active_plugin(void);

namespace cdc::plugin_manager {

namespace {

cdc::ui::ConfirmView::Icon toConfirmIcon(uint8_t icon)
{
    switch (icon) {
        case UI_ICON_ERROR: return cdc::ui::ConfirmView::Icon::ERROR;
        case UI_ICON_ALERT: return cdc::ui::ConfirmView::Icon::WARNING;
        default:            return cdc::ui::ConfirmView::Icon::QUESTION;
    }
}

// Single recursive mutex shared by every plugin list view. Only one plugin
// list is the current top view at a time, and all edits target that list, so
// one process-lifetime lock suffices. It serialises the plg_tick-task buffer
// swaps against the UI-task render/key reads (see ListView::setEditMutex).
SemaphoreHandle_t listEditMutex()
{
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

// Same idea for the plugin canvas: host calls on the plg_tick task mutate the
// display list and arenas while the UI task replays them in render(). One
// process-lifetime lock, since only one plugin canvas exists at a time.
SemaphoreHandle_t canvasEditMutex()
{
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

}  // namespace

PluginUiState& PluginUiState::instance() noexcept
{
    static PluginUiState s;
    return s;
}

// --- View-callback trampolines ---------------------------------------------

void PluginUiState::onListSelect(uint16_t index, void* userData)
{
    auto* state = static_cast<ListState*>(userData);
    if (!state) {
        auto& s = instance();
        state = s.list_.get();
    }
    if (!state) return;
    uint32_t item_id = (index < state->count && state->item_ids)
                       ? state->item_ids[index] : 0;
    uint32_t action  = state->select_action_id;
    PluginManager::instance().dispatchAction(action, index, item_id);
}

int PluginUiState::setViewEmpty(const char* text)
{
    if (!list_ || !list_->view) return HOST_ERR_NOT_FOUND;
    if (!text || *text == '\0') {
        list_->view->setEmptyText(nullptr);
        list_->empty_buf.reset();
        return HOST_OK;
    }
    std::string cp = toDisplay(text);
    auto buf = psramAlloc<char>(cp.size() + 1);
    if (!buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(buf.get(), cp.c_str(), cp.size() + 1);
    list_->view->setEmptyText(buf.get());
    list_->empty_buf = std::move(buf);
    return HOST_OK;
}

int PluginUiState::setViewFooter(const char* hint)
{
    auto* top = cdc::ui::ViewStack::instance().current();
    if (!top) return HOST_ERR_NOT_FOUND;

    PsramUniquePtr<char>* slot = nullptr;
    if (list_ && list_->view && top == list_->view.get())        slot = &list_->footer_buf;
    else if (confirm_.view && top == confirm_.view.get())        slot = &confirm_.footer_buf;
    else if (input_.t9_view && top == input_.t9_view.get())      slot = &input_.footer_buf;
    else if (input_.pin_view && top == input_.pin_view.get())    slot = &input_.footer_buf;
    else if (input_.slider_view && top == input_.slider_view.get()) slot = &input_.footer_buf;
    else if (input_.date_view && top == input_.date_view.get())  slot = &input_.footer_buf;
    else if (input_.time_view && top == input_.time_view.get())  slot = &input_.footer_buf;
    else if (input_.color_view && top == input_.color_view.get()) slot = &input_.footer_buf;
    else if (canvas_.view && top == canvas_.view.get())          slot = &canvas_.footer_buf;
    if (!slot) return HOST_ERR_NOT_FOUND;

    if (!hint || *hint == '\0') {
        top->setFooterHint(nullptr);
        slot->reset();
        return HOST_OK;
    }
    std::string cp = toDisplay(hint);
    auto buf = psramAlloc<char>(cp.size() + 1);
    if (!buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(buf.get(), cp.c_str(), cp.size() + 1);
    top->setFooterHint(buf.get());
    *slot = std::move(buf);
    return HOST_OK;
}

int PluginUiState::setViewLifecycle(uint32_t hide_action_id, uint32_t show_action_id)
{
    // setLifecycleHooks is a virtual IView no-op overridden by ViewBase, so the
    // top view stores the hooks itself - no need to enumerate plugin view types.
    auto* top = cdc::ui::ViewStack::instance().current();
    if (!top) return HOST_ERR_NOT_FOUND;

    lifecycle_hide_action_ = hide_action_id;
    lifecycle_show_action_ = show_action_id;
    top->setLifecycleHooks(hide_action_id ? &onViewHide : nullptr,
                           show_action_id ? &onViewShow : nullptr, nullptr);
    return HOST_OK;
}

void PluginUiState::resetForPluginStop()
{
    // Retract this plugin's own modal overlays (context menu / confirm) before
    // the backing view objects are reset below. showModal() stores a raw pointer
    // to these members, so one left on the stack would dangle and crash on the
    // next render once it is freed here. removeModal targets only these specific
    // views from anywhere in the stack, leaving system modals untouched.
    auto& vs = cdc::ui::ViewStack::instance();
    vs.removeModal(ctxmenu_.view.get());
    vs.removeModal(confirm_.view.get());

    list_.reset();
    list_graveyard_.clear();
    ctxmenu_           = ContextMenuState{};
    confirm_           = ConfirmState{};
    input_             = InputState{};
    canvas_            = CanvasState{};
    exclusive_token_   = nullptr;
    inactivity_action_ = 0;
}

void PluginUiState::onListMenu(uint16_t index, void* userData)
{
    auto* state = static_cast<ListState*>(userData);
    if (!state) {
        auto& s = instance();
        state = s.list_.get();
    }
    if (!state || state->menu_action_id == 0) return;
    uint32_t item_id = (index < state->count && state->item_ids)
                       ? state->item_ids[index] : 0;
    PluginManager::instance().dispatchAction(state->menu_action_id, index, item_id);
}

void PluginUiState::onConfirmYes(void*)
{
    auto& s = instance();
    uint32_t action = s.confirm_.action_id;
    s.confirm_.action_id = 0;
    PluginManager::instance().dispatchAction(action, 0, 1);
}

void PluginUiState::onConfirmNo(void*)
{
    auto& s = instance();
    uint32_t action = s.confirm_.action_id;
    s.confirm_.action_id = 0;
    PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onT9Save(const char* text)
{
    auto& s = instance();
    s.input_.last_text = text ? text : "";
    uint32_t action = s.input_.action_id;
    auto len = static_cast<uint32_t>(s.input_.last_text.size());
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, len, 1);
}

bool PluginUiState::onPinVerify(const char* pin)
{
    auto& s = instance();
    s.input_.last_text = pin ? pin : "";
    uint32_t action = s.input_.action_id;
    auto len = static_cast<uint32_t>(s.input_.last_text.size());
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, len, 1);
    return true;
}

void PluginUiState::onSliderSave(uint16_t value)
{
    auto& s = instance();
    s.input_.last_int = static_cast<int32_t>(value);
    s.input_.has_int = true;
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, value, 1);
}

void PluginUiState::onDateSave(uint8_t day, uint8_t month, uint16_t year)
{
    auto& s = instance();
    s.input_.last_date = (static_cast<uint32_t>(year)  << 16) |
                        (static_cast<uint32_t>(month) <<  8) |
                         static_cast<uint32_t>(day);
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, s.input_.last_date, 1);
}

void PluginUiState::onTimeSave(uint8_t hour, uint8_t minute)
{
    auto& s = instance();
    s.input_.last_time = static_cast<uint16_t>((hour << 8) | minute);
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, s.input_.last_time, 1);
}

void PluginUiState::onColorSave(uint8_t r, uint8_t g, uint8_t b)
{
    auto& s = instance();
    uint32_t packed = (static_cast<uint32_t>(r) << 16)
                    | (static_cast<uint32_t>(g) << 8)
                    |  static_cast<uint32_t>(b);
    s.input_.last_int = static_cast<int32_t>(packed);
    s.input_.has_int = true;
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    PluginManager::instance().dispatchAction(action, packed, 1);
}

void PluginUiState::onInputCancel()
{
    // Cancel path for the self-popping input views (T9, slider, date, time,
    // color): the view already popped itself, so just report the dismissal with
    // idx = 0 and user_data = 0 (the confirm path uses user_data = 1).
    auto& s = instance();
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    if (action) PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onPinCancel()
{
    // PinEntryView leaves dismissal to its cancel callback, so pop it here
    // before reporting, mirroring host_ui_pop semantics for the other inputs.
    cdc::ui::ViewStack::instance().pop();
    auto& s = instance();
    uint32_t action = s.input_.action_id;
    s.input_.action_id = 0;
    if (action) PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onInactivity()
{
    uint32_t action = instance().inactivity_action_;
    if (action) PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onViewHide(void* /*userData*/)
{
    uint32_t action = instance().lifecycle_hide_action_;
    if (action) PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onViewShow(void* /*userData*/)
{
    uint32_t action = instance().lifecycle_show_action_;
    if (action) PluginManager::instance().dispatchAction(action, 0, 0);
}

void PluginUiState::onCanvasKey(char key, uint32_t focused_widget)
{
    uint32_t action = instance().canvas_.key_action_id;
    if (action) {
        PluginManager::instance().dispatchAction(
            action, focused_widget, static_cast<uint32_t>(static_cast<unsigned char>(key)));
    }
}

void PluginUiState::onCanvasLongPress(char key)
{
    uint32_t action = instance().canvas_.long_press_action_id;
    if (action) {
        PluginManager::instance().dispatchAction(
            action, 0, static_cast<uint32_t>(static_cast<unsigned char>(key)));
    }
}

void PluginUiState::onCanvasWidget(uint32_t widget_id, cdc::ui::CanvasView::WidgetEvent event)
{
    uint32_t action = instance().canvas_.widget_action_id;
    if (action) {
        PluginManager::instance().dispatchAction(
            action, widget_id, static_cast<uint32_t>(event));
    }
}

void PluginUiState::onCanvasAnim(uint32_t action_id, uint32_t handle, uint32_t ref_id)
{
    // The action id was supplied per tween/sprite playback (done_action_id),
    // so it goes out verbatim: plugin_on_action(action_id, handle, ref_id).
    if (action_id) {
        PluginManager::instance().dispatchAction(action_id, handle, ref_id);
    }
}

// --- View push API ----------------------------------------------------------

namespace {

void ctxCb0() { PluginUiState::instance().dispatchContextSelect(0); }
void ctxCb1() { PluginUiState::instance().dispatchContextSelect(1); }
void ctxCb2() { PluginUiState::instance().dispatchContextSelect(2); }
void ctxCb3() { PluginUiState::instance().dispatchContextSelect(3); }
void ctxCb4() { PluginUiState::instance().dispatchContextSelect(4); }
void ctxCb5() { PluginUiState::instance().dispatchContextSelect(5); }
void ctxCb6() { PluginUiState::instance().dispatchContextSelect(6); }
void ctxCb7() { PluginUiState::instance().dispatchContextSelect(7); }

constexpr void (*kCtxCallbacks[cdc::ui::ContextMenuView::MAX_ITEMS])() = {
    &ctxCb0, &ctxCb1, &ctxCb2, &ctxCb3,
    &ctxCb4, &ctxCb5, &ctxCb6, &ctxCb7,
};

}  // namespace

void PluginUiState::dispatchContextSelect(uint8_t idx)
{
    if (idx >= ctxmenu_.count) return;
    uint32_t item_id = ctxmenu_.item_ids ? ctxmenu_.item_ids[idx] : 0;
    uint32_t action  = ctxmenu_.select_action_id;
    PluginManager::instance().dispatchAction(action, idx, item_id);
}

int PluginUiState::pushContextMenu(const char* title, const ui_item_t* items, uint16_t count,
                                    uint32_t select_action_id)
{
    if (count == 0 || !items) return HOST_ERR_INVALID_ARG;
    if (count > cdc::ui::ContextMenuView::MAX_ITEMS) return HOST_ERR_INVALID_ARG;

    ContextMenuState next;
    std::string cpLabels[cdc::ui::ContextMenuView::MAX_ITEMS];
    size_t pool_bytes = 0;
    for (uint16_t i = 0; i < count; ++i) {
        cpLabels[i] = toDisplay(items[i].label);
        pool_bytes += cpLabels[i].size() + 1;
    }
    next.items       = psramAlloc<cdc::ui::ContextMenuItem>(count);
    next.string_pool = psramAlloc<char>(pool_bytes + 1);
    next.item_ids    = psramAlloc<uint32_t>(count);
    if (!next.items || !next.string_pool || !next.item_ids) {
        return HOST_ERR_NO_MEMORY;
    }

    char* dst = next.string_pool.get();
    for (uint16_t i = 0; i < count; ++i) {
        size_t n = cpLabels[i].size();
        std::memcpy(dst, cpLabels[i].c_str(), n);
        dst[n] = '\0';
        next.items[i].label    = dst;
        next.items[i].callback = kCtxCallbacks[i];
        next.item_ids[i]       = items[i].item_id;
        dst += n + 1;
    }
    next.count = count;
    next.select_action_id = select_action_id;
    if (title && *title) {
        std::string cpTitle = toDisplay(title);
        next.title_buf = psramAlloc<char>(cpTitle.size() + 1);
        if (!next.title_buf) return HOST_ERR_NO_MEMORY;
        std::memcpy(next.title_buf.get(), cpTitle.c_str(), cpTitle.size() + 1);
    }
    next.view = std::make_unique<cdc::ui::ContextMenuView>();
    next.view->init(next.title_buf ? next.title_buf.get() : "",
                    next.items.get(), static_cast<uint8_t>(count));

    cdc::ui::ViewStack::instance().showModal(next.view.get());
    ctxmenu_ = std::move(next);
    return HOST_OK;
}

int PluginUiState::pushList(const char* title, const ui_item_t* items, uint16_t count,
                            uint32_t select_action_id, uint32_t menu_action_id,
                            bool replace_top)
{
    if (!title || (!items && count > 0)) return HOST_ERR_INVALID_ARG;

    auto next = std::make_unique<ListState>();
    if (count > 0) {
        next->items    = psramAlloc<cdc::ui::ListItem>(count);
        next->item_ids = psramAlloc<uint32_t>(count);
        if (!next->items || !next->item_ids) {
            return HOST_ERR_NO_MEMORY;
        }
        next->labels.reserve(count);
    }
    next->capacity = count;

    for (uint16_t i = 0; i < count; ++i) {
        std::string cp = toDisplay(items[i].label);
        auto buf = psramAlloc<char>(cp.size() + 1);
        if (!buf) return HOST_ERR_NO_MEMORY;
        std::memcpy(buf.get(), cp.c_str(), cp.size() + 1);
        next->items[i].label        = buf.get();
        next->items[i].icon         = items[i].icon;
        next->items[i].iconDisabled = items[i].icon_disabled;
        next->items[i].userData     = next.get();
        next->item_ids[i]           = items[i].item_id;
        next->labels.push_back(std::move(buf));
    }
    next->count            = count;
    next->select_action_id = select_action_id;
    next->menu_action_id   = menu_action_id;
    std::string cpTitle = toDisplay(title);
    next->title_buf  = psramAlloc<char>(cpTitle.size() + 1);
    if (!next->title_buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(next->title_buf.get(), cpTitle.c_str(), cpTitle.size() + 1);
    next->view = std::make_unique<cdc::ui::ListView>();
    next->view->setEditMutex(listEditMutex());
    next->view->init(next->title_buf.get(), next->items.get(), count);
    next->view->setOnSelect(&PluginUiState::onListSelect);
    if (menu_action_id != 0) next->view->setOnMenu(&PluginUiState::onListMenu);

    auto& stack = cdc::ui::ViewStack::instance();
    const bool can_replace = (list_ && list_->view && stack.current() == list_->view.get());
    if (can_replace && replace_top) {
        stack.replace(next->view.get());
    } else {
        stack.push(next->view.get());
    }
    if (list_) {
        list_graveyard_.push_back(std::move(list_));
    }
    list_ = std::move(next);
    return HOST_OK;
}

int PluginUiState::updateListItem(uint16_t index, const ui_item_t* item)
{
    if (!item) return HOST_ERR_INVALID_ARG;
    if (!list_ || !list_->view || !list_->items || !list_->item_ids) {
        return HOST_ERR_NOT_FOUND;
    }
    if (index >= list_->count) return HOST_ERR_INVALID_ARG;

    // Only mutate while our list is the active top view.
    if (cdc::ui::ViewStack::instance().current() != list_->view.get()) {
        return HOST_ERR_NOT_FOUND;
    }

    // The packed string_pool labels cannot grow in place, so the new label
    // lives in a per-item override buffer owned by this ListState.
    std::string cp = toDisplay(item->label);
    auto buf = psramAlloc<char>(cp.size() + 1);
    if (!buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(buf.get(), cp.c_str(), cp.size() + 1);

    {
        // Hold the list edit lock so the UI task does not read items_[index]
        // while its label pointer is being re-pointed at the new buffer.
        cdc::core::RecursiveMutexGuard guard(listEditMutex());
        list_->items[index].label        = buf.get();
        list_->items[index].icon         = item->icon;
        list_->items[index].iconDisabled = item->icon_disabled;
        list_->item_ids[index]           = item->item_id;
        list_->labels[index]             = std::move(buf);

        list_->view->updateItem(index);
    }
    return HOST_OK;
}

bool PluginUiState::growList(uint16_t need)
{
    if (!list_) return false;
    if (list_->capacity >= need) return true;

    uint16_t newCap = static_cast<uint16_t>(list_->capacity + list_->capacity / 2 + 8);
    if (newCap < need) newCap = need;
    if (newCap > cdc::ui::ListView::MAX_ITEMS) newCap = cdc::ui::ListView::MAX_ITEMS;

    auto ni  = psramAlloc<cdc::ui::ListItem>(newCap);
    auto nid = psramAlloc<uint32_t>(newCap);
    if (!ni || !nid) return false;
    for (uint16_t i = 0; i < list_->count; ++i) {
        ni[i]  = list_->items[i];
        nid[i] = list_->item_ids[i];
    }
    list_->items    = std::move(ni);
    list_->item_ids = std::move(nid);
    list_->capacity = newCap;
    // The backing array moved, so the view must be re-pointed. This is the only
    // init on the insert path and amortises to O(log count) over many inserts.
    list_->view->preservePosition();
    list_->view->init(list_->title_buf ? list_->title_buf.get() : "",
                      list_->items.get(), list_->count);
    return true;
}

int PluginUiState::insertListItem(uint16_t index, const ui_item_t* item)
{
    if (!item) return HOST_ERR_INVALID_ARG;
    if (!list_ || !list_->view) {
        return HOST_ERR_NOT_FOUND;
    }
    if (cdc::ui::ViewStack::instance().current() != list_->view.get()) {
        return HOST_ERR_NOT_FOUND;
    }
    const uint16_t oldCount = list_->count;
    if (oldCount >= cdc::ui::ListView::MAX_ITEMS) return HOST_ERR_NO_MEMORY;
    if (index > oldCount) index = oldCount;

    std::string cp = toDisplay(item->label);
    auto buf = psramAlloc<char>(cp.size() + 1);
    if (!buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(buf.get(), cp.c_str(), cp.size() + 1);

    {
        // Serialise with the UI task: it must not read items_ mid-shift.
        cdc::core::RecursiveMutexGuard guard(listEditMutex());
        if (!growList(static_cast<uint16_t>(oldCount + 1))) return HOST_ERR_NO_MEMORY;
        // Shift rows [index, oldCount) up by one within the capacity array.
        for (uint16_t r = oldCount; r > index; --r) {
            list_->items[r]    = list_->items[r - 1];
            list_->item_ids[r] = list_->item_ids[r - 1];
        }
        list_->labels.insert(list_->labels.begin() + index, std::move(buf));
        list_->items[index].label        = list_->labels[index].get();
        list_->items[index].icon         = item->icon;
        list_->items[index].iconDisabled = item->icon_disabled;
        list_->items[index].userData     = list_.get();
        list_->item_ids[index]           = item->item_id;
        list_->count = static_cast<uint16_t>(oldCount + 1);
        list_->view->insertItem(index);
    }
    return HOST_OK;
}

int PluginUiState::removeListItem(uint16_t index)
{
    if (!list_ || !list_->view || !list_->items || !list_->item_ids) {
        return HOST_ERR_NOT_FOUND;
    }
    if (cdc::ui::ViewStack::instance().current() != list_->view.get()) {
        return HOST_ERR_NOT_FOUND;
    }
    const uint16_t oldCount = list_->count;
    if (index >= oldCount) return HOST_ERR_INVALID_ARG;

    {
        // Serialise with the UI task: it must not read items_ mid-shift.
        cdc::core::RecursiveMutexGuard guard(listEditMutex());
        list_->labels.erase(list_->labels.begin() + index);
        // Shift rows (index, oldCount) down by one within the capacity array.
        for (uint16_t r = index; r + 1 < oldCount; ++r) {
            list_->items[r]    = list_->items[r + 1];
            list_->item_ids[r] = list_->item_ids[r + 1];
        }
        list_->count = static_cast<uint16_t>(oldCount - 1);
        list_->view->removeItem(index);
    }
    return HOST_OK;
}

int PluginUiState::pushConfirm(const char* text, uint8_t icon, uint32_t action_id)
{
    if (!text) return HOST_ERR_INVALID_ARG;
    confirm_           = ConfirmState{};
    confirm_.view      = std::make_unique<cdc::ui::ConfirmView>();
    confirm_.action_id = action_id;
    std::string cpText = toDisplay(text);
    confirm_.view->init(cpText.c_str(), toConfirmIcon(icon));
    confirm_.view->setOnConfirm(&PluginUiState::onConfirmYes, nullptr);
    confirm_.view->setOnCancel (&PluginUiState::onConfirmNo,  nullptr);
    cdc::ui::ViewStack::instance().showModal(confirm_.view.get());
    return HOST_OK;
}

int PluginUiState::pushT9(const char* title, const char* initial,
                          uint16_t max_len, uint32_t action_id)
{
    if (!title || max_len == 0) return HOST_ERR_INVALID_ARG;
    input_           = InputState{};
    input_.action_id = action_id;
    std::string cpTitle = toDisplay(title);
    input_.title_buf = psramAlloc<char>(cpTitle.size() + 1);
    if (!input_.title_buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(input_.title_buf.get(), cpTitle.c_str(), cpTitle.size() + 1);
    std::string cpInitial = toDisplay(initial);
    input_.t9_view   = std::make_unique<cdc::ui::T9InputView>();
    input_.t9_view->init(input_.title_buf.get(), initial ? cpInitial.c_str() : nullptr, max_len);
    input_.t9_view->setOnSave(&PluginUiState::onT9Save);
    input_.t9_view->setOnCancel(&PluginUiState::onInputCancel);
    cdc::ui::ViewStack::instance().push(input_.t9_view.get());
    return HOST_OK;
}

int PluginUiState::pushPin(const char* title, uint8_t max_len, uint8_t max_attempts,
                           uint32_t action_id)
{
    if (!title || max_len == 0) return HOST_ERR_INVALID_ARG;
    input_           = InputState{};
    input_.action_id = action_id;
    std::string cpTitle = toDisplay(title);
    input_.pin_view  = std::make_unique<cdc::ui::PinEntryView>();
    input_.pin_view->init(cpTitle.c_str(), max_len, max_attempts);
    input_.pin_view->setOnVerify(&PluginUiState::onPinVerify);
    input_.pin_view->setOnCancel(&PluginUiState::onPinCancel);
    cdc::ui::ViewStack::instance().push(input_.pin_view.get());
    return HOST_OK;
}

int PluginUiState::pushSlider(const char* title, int32_t min, int32_t max, int32_t init,
                              int32_t step, const char* unit, uint32_t action_id)
{
    if (!title || min >= max) return HOST_ERR_INVALID_ARG;
    input_             = InputState{};
    input_.action_id   = action_id;
    std::string cpTitle = toDisplay(title);
    input_.title_buf = psramAlloc<char>(cpTitle.size() + 1);
    if (!input_.title_buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(input_.title_buf.get(), cpTitle.c_str(), cpTitle.size() + 1);
    std::string cpUnit = toDisplay(unit);
    input_.unit_buf = psramAlloc<char>(cpUnit.size() + 1);
    if (!input_.unit_buf) return HOST_ERR_NO_MEMORY;
    std::memcpy(input_.unit_buf.get(), cpUnit.c_str(), cpUnit.size() + 1);
    input_.slider_view = std::make_unique<cdc::ui::SliderView>();
    input_.slider_view->init(input_.title_buf.get(), min, max, init, step, input_.unit_buf.get());
    input_.slider_view->setOnSave(&PluginUiState::onSliderSave);
    input_.slider_view->setOnCancel(&PluginUiState::onInputCancel);
    cdc::ui::ViewStack::instance().push(input_.slider_view.get());
    return HOST_OK;
}

int PluginUiState::pushDate(const char* title, uint8_t d, uint8_t m, uint16_t y,
                            uint32_t action_id)
{
    if (!title) return HOST_ERR_INVALID_ARG;
    input_           = InputState{};
    input_.action_id = action_id;
    std::string cpTitle = toDisplay(title);
    input_.date_view = std::make_unique<cdc::ui::DateInputView>();
    input_.date_view->init(cpTitle.c_str(), d, m, y);
    input_.date_view->setOnConfirm(&PluginUiState::onDateSave);
    input_.date_view->setOnCancel(&PluginUiState::onInputCancel);
    cdc::ui::ViewStack::instance().push(input_.date_view.get());
    return HOST_OK;
}

int PluginUiState::pushTime(const char* title, uint8_t h, uint8_t m, uint32_t action_id)
{
    if (!title) return HOST_ERR_INVALID_ARG;
    input_           = InputState{};
    input_.action_id = action_id;
    std::string cpTitle = toDisplay(title);
    input_.time_view = std::make_unique<cdc::ui::TimeInputView>();
    input_.time_view->init(cpTitle.c_str(), h, m);
    input_.time_view->setOnConfirm(&PluginUiState::onTimeSave);
    input_.time_view->setOnCancel(&PluginUiState::onInputCancel);
    cdc::ui::ViewStack::instance().push(input_.time_view.get());
    return HOST_OK;
}

int PluginUiState::pushColorPicker(uint8_t r, uint8_t g, uint8_t b, uint32_t action_id)
{
    input_            = InputState{};
    input_.action_id  = action_id;
    input_.color_view = std::make_unique<cdc::ui::ColorPickerView>();
    input_.color_view->init(r, g, b);
    input_.color_view->setOnSave(&PluginUiState::onColorSave);
    input_.color_view->setOnCancel(&PluginUiState::onInputCancel);
    cdc::ui::ViewStack::instance().push(input_.color_view.get());
    return HOST_OK;
}

int PluginUiState::pushCanvas(const char* title, uint32_t key_action_id,
                               uint32_t widget_action_id)
{
    canvas_ = CanvasState{};

    if (title && *title) {
        std::string cp = toDisplay(title);
        canvas_.title_buf = psramAlloc<char>(cp.size() + 1);
        if (!canvas_.title_buf) return HOST_ERR_NO_MEMORY;
        std::memcpy(canvas_.title_buf.get(), cp.c_str(), cp.size() + 1);
    }

    canvas_.key_action_id    = key_action_id;
    canvas_.widget_action_id = widget_action_id;
    canvas_.view             = std::make_unique<cdc::ui::CanvasView>();
    canvas_.view->setEditMutex(canvasEditMutex());
    canvas_.view->init(canvas_.title_buf.get());
    canvas_.view->setKeyCallback(&PluginUiState::onCanvasKey);
    canvas_.view->setWidgetCallback(&PluginUiState::onCanvasWidget);
    canvas_.view->setAnimCallback(&PluginUiState::onCanvasAnim);

    cdc::ui::ViewStack::instance().push(canvas_.view.get());
    return HOST_OK;
}

int PluginUiState::setCanvasLongPressAction(uint32_t action_id)
{
    if (!canvas_.view) return HOST_ERR_NOT_FOUND;
    canvas_.long_press_action_id = action_id;
    canvas_.view->setLongPressCallback(action_id ? &PluginUiState::onCanvasLongPress : nullptr);
    return HOST_OK;
}

cdc::ui::CanvasView* PluginUiState::canvasView()
{
    return canvas_.view.get();
}

int PluginUiState::acquireExclusive()
{
    void* plugin = plg_get_active_plugin();
    if (!plugin) return HOST_ERR_NO_CAPABILITY;
    exclusive_token_ = plugin;
    return cdc::ui::ViewStack::instance().acquireExclusive(plugin) ? HOST_OK : HOST_ERR_BUSY;
}

int PluginUiState::releaseExclusive()
{
    if (!exclusive_token_) return HOST_ERR_NOT_FOUND;
    bool ok = cdc::ui::ViewStack::instance().releaseExclusive(exclusive_token_);
    exclusive_token_ = nullptr;
    return ok ? HOST_OK : HOST_ERR_GENERIC;
}

int PluginUiState::setInactivity(uint32_t timeout_ms, uint32_t action_id)
{
    inactivity_action_ = action_id;
    cdc::ui::ViewStack::instance().setInactivityTimeout(
        action_id ? &PluginUiState::onInactivity : nullptr,
        action_id ? timeout_ms : 0);
    return HOST_OK;
}

int PluginUiState::consumeInputText(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    int n = copyUtf8(input_.last_text.c_str(), out, out_size);
    input_.last_text.clear();
    return n;
}

int PluginUiState::consumeInputInt(int32_t* out)
{
    if (!out) return HOST_ERR_INVALID_ARG;
    if (!input_.has_int) return HOST_ERR_NOT_FOUND;  // no int input pending
    *out = input_.last_int;
    input_.last_int = 0;
    input_.has_int = false;
    return HOST_OK;
}

}  // namespace cdc::plugin_manager
