/**
 * \file VfatExplorerView.h
 * \brief GUI file explorer for the plugins FAT partition.
 *
 * A single view that walks the partition in place: selecting a folder descends
 * into it, the back key ascends to the parent (and pops out at the root). Text
 * files open in an InfoView and can be edited via T9; the context menu also
 * offers Delete and Add (folder/file). The footer shows the partition size and
 * free space in KiB.
 */

#pragma once

#include "cdc_ui/IView.h"
#include "cdc_views/ListView.h"
#include "VfatFs.h"

#include <string>
#include <vector>

namespace cdc::mod_vfat {

class VfatExplorerView : public cdc::ui::IView {
public:
    static VfatExplorerView& instance();

    /// Reset to the partition root. Call before pushing the view.
    void openRoot();

    // IView
    void onEnter(void* context) override;
    void onExit() override;
    void onResume() override;
    void render(bool partial) override;
    bool needsRender() const override;
    void markDirty() override;
    void clearDirty() override;
    cdc::ui::InputResult onKey(char key) override;
    const char* getName() const override { return "VfatExplorer"; }
    const char* getFooterHint() const override { return footer_; }

    // Context-menu actions on the stashed selection / current directory.
    void openSelected();
    void editSelected();
    void deleteSelected();
    void doDelete();
    void addEntry();
    void beginAdd(bool isDir);

private:
    enum class T9Mode : uint8_t { Edit, AddDirName, AddFileName, AddFileContent };

    VfatExplorerView() = default;

    void rebuild();
    void descend(const std::string& name);
    void ascend();
    void openEntry(uint16_t index);
    std::string relOf(const std::string& name) const;

    static void onSelectCb(uint16_t index, void* userData);
    static void onMenuCb(uint16_t index, void* userData);
    static void onT9SaveCb(const char* text);

    cdc::ui::ListView              list_;
    std::string                    cwd_;       // relative to root ("" = root)
    std::vector<FsEntry>           entries_;
    std::vector<cdc::ui::ListItem> items_;
    std::vector<std::string>       labels_;
    uint16_t                       menuSel_ = 0xFFFF;
    T9Mode                         t9Mode_  = T9Mode::Edit;
    std::string                    editPath_;  // file being edited in T9
    std::string                    newName_;   // pending new file name (Add)
    std::string                    delTarget_; // entry awaiting delete confirm
    bool                           delIsDir_ = false;
    char                           titleBuf_[64] = {0};
    char                           footer_[40]   = {0};
    char                           t9Title_[64]  = {0};
};

}  // namespace cdc::mod_vfat
