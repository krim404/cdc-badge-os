/**
 * \file VfatExplorerView.cpp
 * \brief Implementation of the vFAT GUI explorer.
 *
 * Files on the partition are canonical UTF-8. The display pipeline is CP437,
 * so file names and contents are converted UTF-8 -> CP437 for display/edit and
 * CP437 -> UTF-8 again before anything is written back.
 */

#include "VfatExplorerView.h"

#include "cdc_views/ContextMenuView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/MarkdownView.h"
#include "cdc_views/ImageView.h"
#include "cdc_views/HtmlViewerHook.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_core/Cp437.h"

#include <cstdio>
#include <cstring>
#include <cctype>

namespace cdc::mod_vfat {

namespace {
// CP437 right-pointer, used as the "enter folder" marker.
constexpr uint8_t ICON_FOLDER = 0x10;
// Name length limit for new files/folders.
constexpr uint16_t kNameMax = 48;

cdc::ui::ContextMenuItem s_ctxItems[5];
cdc::ui::T9InputView     s_t9;

// ConfirmView hides its modal before invoking this, so it only acts.
void onConfirmYes(void*) { VfatExplorerView::instance().doDelete(); }

void onCtxOpen()    { VfatExplorerView::instance().openSelected(); }
void onCtxEdit()    { VfatExplorerView::instance().editSelected(); }
void onCtxDelete()  { VfatExplorerView::instance().deleteSelected(); }
void onCtxAdd()     { VfatExplorerView::instance().addEntry(); }
void onCtxNewDir()  { VfatExplorerView::instance().beginAdd(true); }
void onCtxNewFile() { VfatExplorerView::instance().beginAdd(false); }

/// Lower-case file extension, or "" when the name has none.
std::string extOf(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

/// Files with a text-like extension - or none - can be viewed as text.
bool isViewable(const std::string& name)
{
    std::string e = extOf(name);
    if (e.empty()) return true;
    static const char* kText[] = {"txt", "json", "md", "csv", "log", "cfg",
                                  "ini", "text", "nfo", "yaml", "yml", "xml",
                                  "html", "htm", "meta", "lang", "markdown", nullptr};
    for (int i = 0; kText[i]; ++i) if (e == kText[i]) return true;
    return false;
}

/// Maximum image source read into RAM before decoding.
constexpr size_t kImageMaxBytes = 512 * 1024;

/// Files with an image extension open in the image viewer.
bool isImage(const std::string& name)
{
    std::string e = extOf(name);
    return e == "png" || e == "jpg" || e == "jpeg";
}

/// Viewable text files are editable, except read-only plugin metadata (.meta).
bool isEditable(const std::string& name)
{
    return isViewable(name) && extOf(name) != "meta";
}
}  // namespace

VfatExplorerView& VfatExplorerView::instance()
{
    static VfatExplorerView inst;
    return inst;
}

void VfatExplorerView::openRoot(bool showSystem)
{
    showSystem_ = showSystem;
    cwd_.clear();
    rebuild();
}

void VfatExplorerView::onEnter(void* /*context*/) { rebuild(); }
void VfatExplorerView::onExit() {}
void VfatExplorerView::onResume() { rebuild(); }

void VfatExplorerView::render(bool partial) { list_.render(partial); }
bool VfatExplorerView::needsRender() const  { return list_.needsRender(); }
void VfatExplorerView::markDirty()          { list_.markDirty(); }
void VfatExplorerView::clearDirty()         { list_.clearDirty(); }

cdc::ui::InputResult VfatExplorerView::onKey(char key)
{
    if (key == cdc::ui::KEY_NO) {
        if (!cwd_.empty()) { ascend(); return cdc::ui::InputResult::CONSUMED; }
        return cdc::ui::InputResult::REQUEST_POP;
    }
    return list_.onKey(key);
}

std::string VfatExplorerView::relOf(const std::string& name) const
{
    return cwd_.empty() ? name : cwd_ + "/" + name;
}

void VfatExplorerView::rebuild()
{
    bool truncated = false;
    fs::list(cwd_, entries_, truncated, showSystem_);

    // Hide the system folder from the user view (root only); the System Files
    // view (showSystem_) keeps it. Filtered from entries_ so list indices stay
    // aligned with the displayed items.
    if (!showSystem_ && cwd_.empty()) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->name == "system") it = entries_.erase(it);
            else ++it;
        }
    }

    labels_.clear();
    items_.clear();
    labels_.reserve(entries_.size());
    items_.reserve(entries_.size());

    for (const auto& e : entries_) {
        std::string label = cdc::core::cp437::fromUtf8(e.name.c_str());
        if (e.is_dir) label += "/";
        labels_.push_back(std::move(label));
        uint8_t icon = e.is_dir ? ICON_FOLDER : uint8_t{0};
        items_.push_back(cdc::ui::ListItem{labels_.back().c_str(), icon, false, nullptr});
    }

    std::string title = cdc::core::cp437::fromUtf8(("/" + cwd_).c_str());
    std::snprintf(titleBuf_, sizeof(titleBuf_), "%s", title.c_str());

    uint32_t totalKB = 0, freeKB = 0;
    if (fs::stats(totalKB, freeKB)) {
        std::snprintf(footer_, sizeof(footer_), "%lu MEM/ %lu FREE",
                      static_cast<unsigned long>(totalKB),
                      static_cast<unsigned long>(freeKB));
    } else {
        footer_[0] = '\0';
    }

    list_.setOnSelect(&onSelectCb);
    list_.setOnMenu(&onMenuCb);
    list_.setEmptyText(cdc::ui::tr("core.empty"));
    list_.setHint(footer_);
    list_.init(titleBuf_, items_.data(), static_cast<uint16_t>(items_.size()));
}

void VfatExplorerView::descend(const std::string& name)
{
    cwd_ = relOf(name);
    rebuild();
}

void VfatExplorerView::ascend()
{
    size_t pos = cwd_.find_last_of('/');
    if (pos == std::string::npos) cwd_.clear();
    else cwd_.erase(pos);
    rebuild();
}

void VfatExplorerView::openEntry(uint16_t index)
{
    if (index >= entries_.size()) return;
    const FsEntry& e = entries_[index];
    if (e.is_dir) { descend(e.name); return; }

    if (isImage(e.name)) {
        std::string raw;
        if (!fs::readText(relOf(e.name), raw, kImageMaxBytes)) {
            cdc::ui::showToastError(cdc::ui::tr("core.failed"), 1500);
            return;
        }
        std::string title = cdc::core::cp437::fromUtf8(e.name.c_str());
        std::snprintf(t9Title_, sizeof(t9Title_), "%s", title.c_str());
        cdc::ui::showImage(t9Title_, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        return;
    }

    // HTML files open in the browser's reader when that module is present.
    {
        const std::string he = extOf(e.name);
        if (he == "html" || he == "htm") {
            if (auto fn = cdc::ui::htmlViewer()) {
                std::string content;
                if (!fs::readText(relOf(e.name), content, 256u * 1024u)) {
                    cdc::ui::showToastError(cdc::ui::tr("core.failed"), 1500);
                    return;
                }
                fn(e.name.c_str(), content.c_str(), content.size());
                return;
            }
            // No HTML viewer registered: fall through to the plain-text view.
        }
    }

    if (!isViewable(e.name)) return;

    const std::string ext = extOf(e.name);
    const bool isMd = (ext == "md" || ext == "markdown");
    const size_t cap = isMd ? static_cast<size_t>(cdc::ui::MarkdownView::MAX_SOURCE)
                            : static_cast<size_t>(cdc::ui::InfoView::MAX_TEXT_LEN) * 2;

    std::string content;
    if (!fs::readText(relOf(e.name), content, cap)) {
        cdc::ui::showToastError(cdc::ui::tr("core.failed"), 1500);
        return;
    }
    std::string title = cdc::core::cp437::fromUtf8(e.name.c_str());
    std::string body  = cdc::core::cp437::fromUtf8(content.c_str());
    std::snprintf(t9Title_, sizeof(t9Title_), "%s", title.c_str());
    if (isMd) {
        mdViewPath_ = relOf(e.name);
        auto* mv = cdc::ui::showMarkdown(t9Title_, body.c_str(), body.size());
        // Editable Markdown persists task-list checkbox toggles back to the file.
        if (mv && isEditable(e.name)) mv->setCheckSave(&onMdCheckSaveCb, this);
    } else {
        cdc::ui::showInfo(t9Title_, body.c_str());
    }
}

void VfatExplorerView::onMdCheckSaveCb(void* userData, const char* cp437Source, size_t /*len*/)
{
    auto* self = static_cast<VfatExplorerView*>(userData);
    if (!self || self->mdViewPath_.empty() || !cp437Source) return;
    std::string utf8 = cdc::core::cp437::toUtf8(cp437Source);
    fs::writeText(self->mdViewPath_, utf8.data(), utf8.size());
}

void VfatExplorerView::openLocalImage(const char* path)
{
    if (!path || !path[0]) return;
    std::string raw;
    if (!fs::readText(relOf(path), raw, kImageMaxBytes)) {
        cdc::ui::showToastError(cdc::ui::tr("core.failed"), 1500);
        return;
    }
    std::string title = cdc::core::cp437::fromUtf8(path);
    std::snprintf(t9Title_, sizeof(t9Title_), "%s", title.c_str());
    cdc::ui::showImage(t9Title_, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

void VfatExplorerView::openSelected()
{
    openEntry(menuSel_);
}

void VfatExplorerView::editSelected()
{
    if (menuSel_ >= entries_.size()) return;
    const FsEntry& e = entries_[menuSel_];
    if (e.is_dir || !isEditable(e.name)) return;

    editPath_ = relOf(e.name);
    std::string content;
    fs::readText(editPath_, content, cdc::ui::T9InputView::MAX_TEXT_LEN * 2);
    std::string cp = cdc::core::cp437::fromUtf8(content.c_str());
    std::string title = cdc::core::cp437::fromUtf8(e.name.c_str());
    std::snprintf(t9Title_, sizeof(t9Title_), "%s", title.c_str());

    t9Mode_ = T9Mode::Edit;
    s_t9.init(t9Title_, cp.c_str(), cdc::ui::T9InputView::MAX_TEXT_LEN);
    s_t9.setOnSave(&onT9SaveCb);
    cdc::ui::ViewStack::instance().push(&s_t9);
}

void VfatExplorerView::deleteSelected()
{
    if (menuSel_ >= entries_.size()) return;
    const FsEntry& e = entries_[menuSel_];

    // A directory can only be removed when empty.
    if (e.is_dir) {
        std::vector<FsEntry> kids;
        bool trunc = false;
        fs::list(relOf(e.name), kids, trunc);
        if (!kids.empty()) {
            cdc::ui::showToastError(cdc::ui::tr("core.not_empty"), 1500);
            return;
        }
    }

    delTarget_ = relOf(e.name);
    delIsDir_  = e.is_dir;
    std::string nm   = cdc::core::cp437::fromUtf8(e.name.c_str());
    std::string body = std::string(cdc::ui::tr("core.delete")) + " " + nm + "?\n" +
                       cdc::ui::tr("core.sure");
    cdc::ui::showConfirm(body.c_str(), &onConfirmYes, nullptr,
                         cdc::ui::ConfirmView::Icon::WARNING);
}

void VfatExplorerView::doDelete()
{
    bool ok = delIsDir_ ? fs::removeDir(delTarget_) : fs::removeFile(delTarget_);
    cdc::ui::showToastInfo(ok ? cdc::ui::tr("core.deleted") : cdc::ui::tr("core.failed"), 1500);
    rebuild();
}

void VfatExplorerView::addEntry()
{
    s_ctxItems[0] = {cdc::ui::tr("core.new_folder"), &onCtxNewDir};
    s_ctxItems[1] = {cdc::ui::tr("core.new_file"),   &onCtxNewFile};
    cdc::ui::showContextMenu(cdc::ui::tr("core.new"), s_ctxItems, 2);
}

void VfatExplorerView::beginAdd(bool isDir)
{
    t9Mode_ = isDir ? T9Mode::AddDirName : T9Mode::AddFileName;
    newName_.clear();
    std::snprintf(t9Title_, sizeof(t9Title_), "%s",
                  isDir ? cdc::ui::tr("core.new_folder") : cdc::ui::tr("core.new_file"));
    s_t9.init(t9Title_, "", kNameMax);
    s_t9.setOnSave(&onT9SaveCb);
    cdc::ui::ViewStack::instance().push(&s_t9);
}

void VfatExplorerView::onT9SaveCb(const char* text)
{
    // The T9 view has already popped itself before this runs. To keep an input
    // step active we push a fresh T9; otherwise we return to the explorer
    // (whose onResume rebuilds the listing). On-screen text is CP437; it is
    // converted back to UTF-8 before being stored so files stay UTF-8.
    auto& self = instance();
    const std::string cpIn = (text ? text : "");
    const std::string utf8 = cdc::core::cp437::toUtf8(cpIn.c_str());

    switch (self.t9Mode_) {
        case T9Mode::Edit: {
            bool ok = fs::writeText(self.editPath_, utf8.data(), utf8.size());
            cdc::ui::showToastInfo(ok ? cdc::ui::tr("core.saved")
                                      : cdc::ui::tr("core.failed"), 1500);
            break;
        }
        case T9Mode::AddDirName: {
            if (utf8.empty()) break;
            if (fs::exists(self.relOf(utf8))) {
                cdc::ui::showToastError(cdc::ui::tr("core.exists"), 1500);
                s_t9.init(self.t9Title_, cpIn.c_str(), kNameMax);
                s_t9.setOnSave(&onT9SaveCb);
                cdc::ui::ViewStack::instance().push(&s_t9);
                return;
            }
            bool ok = fs::makeDir(self.relOf(utf8));
            cdc::ui::showToastInfo(ok ? cdc::ui::tr("core.saved")
                                      : cdc::ui::tr("core.failed"), 1500);
            break;
        }
        case T9Mode::AddFileName: {
            if (utf8.empty()) break;
            if (fs::exists(self.relOf(utf8))) {
                cdc::ui::showToastError(cdc::ui::tr("core.exists"), 1500);
                s_t9.init(self.t9Title_, cpIn.c_str(), kNameMax);
                s_t9.setOnSave(&onT9SaveCb);
                cdc::ui::ViewStack::instance().push(&s_t9);
                return;
            }
            self.newName_ = utf8;
            self.t9Mode_  = T9Mode::AddFileContent;
            std::string title = cdc::core::cp437::fromUtf8(utf8.c_str());
            std::snprintf(self.t9Title_, sizeof(self.t9Title_), "%s", title.c_str());
            s_t9.init(self.t9Title_, "", cdc::ui::T9InputView::MAX_TEXT_LEN);
            s_t9.setOnSave(&onT9SaveCb);
            cdc::ui::ViewStack::instance().push(&s_t9);
            return;
        }
        case T9Mode::AddFileContent: {
            bool ok = fs::writeText(self.relOf(self.newName_), utf8.data(), utf8.size());
            cdc::ui::showToastInfo(ok ? cdc::ui::tr("core.saved")
                                      : cdc::ui::tr("core.failed"), 1500);
            break;
        }
    }
}

void VfatExplorerView::onSelectCb(uint16_t index, void* /*userData*/)
{
    instance().openEntry(index);
}

void VfatExplorerView::onMenuCb(uint16_t index, void* /*userData*/)
{
    auto& self = instance();
    self.menuSel_ = index;

    uint8_t n = 0;
    if (index < self.entries_.size()) {
        const FsEntry& e = self.entries_[index];
        if (e.is_dir || isViewable(e.name)) s_ctxItems[n++] = {cdc::ui::tr("core.open"), &onCtxOpen};
        if (!e.is_dir && isEditable(e.name)) s_ctxItems[n++] = {cdc::ui::tr("core.edit"), &onCtxEdit};
        s_ctxItems[n++] = {cdc::ui::tr("core.delete"), &onCtxDelete};
    }
    s_ctxItems[n++] = {cdc::ui::tr("core.add"), &onCtxAdd};
    cdc::ui::showContextMenu(cdc::ui::tr("core.actions"), s_ctxItems, n);
}

}  // namespace cdc::mod_vfat
