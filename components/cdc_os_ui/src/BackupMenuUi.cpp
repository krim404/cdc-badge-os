/**
 * \file
 * \brief Expert-menu Backup UI: Export / Import / Delete flows.
 *
 * Export collects a passphrase via T9 twice (with a match check), encrypts a
 * semantic backup to the vFAT partition and toasts the result. Import reads the
 * single on-device file (or toasts when absent), decrypts best-effort and shows
 * a per-section summary. Delete confirms, then removes the file.
 */

#include "AppUiInternal.h"
#include "cdc_os_ui/BackupManager.h"

#include "cdc_views/PasswordT9View.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"

#include <mbedtls/platform_util.h>

#include <cstdio>
#include <cstring>

namespace cdc::ui {

namespace {

/// Maximum passphrase length accepted by the T9 entry flows.
constexpr uint16_t PASSPHRASE_MAX = 64;

/// Static views and cross-step state for the passphrase wizards. The T9 save
/// callback carries no user data, so the in-progress passphrase is held here
/// between the first and second entry and wiped once consumed.
static ListView* s_backupMenu = nullptr;
static ListItem s_backupItems[3];
static PasswordT9View s_passInput;
static char s_firstPass[PASSPHRASE_MAX + 1] = {0};
static char s_summaryText[InfoView::MAX_TEXT_LEN] = {0};

void wipeFirstPass() {
    mbedtls_platform_zeroize(s_firstPass, sizeof(s_firstPass));
}

void pushPassphrase(const char* title, T9InputView::SaveCallback onSave) {
    s_passInput.init(title, nullptr, PASSPHRASE_MAX);
    s_passInput.setOnSave(onSave);
    ViewStack::instance().push(&s_passInput);
}

// --- Export ---------------------------------------------------------------

void onExportConfirm(const char* text) {
    if (!text || std::strcmp(text, s_firstPass) != 0) {
        wipeFirstPass();
        showToastError(ui::tr("core.pins_dont_match"), TOAST_DURATION_MEDIUM_MS);
        return;
    }

    showToastTask(ui::tr("core.task_working"), 0);
    bool ok = os_ui::BackupManager::instance().exportTo(s_firstPass);
    wipeFirstPass();
    ViewStack::instance().hideModal();

    showToast(ok ? ui::tr("core.backup_export_ok") : ui::tr("core.failed"),
              ok ? TOAST_DURATION_MEDIUM_MS : TOAST_DURATION_LONG_MS);
}

void onExportFirst(const char* text) {
    if (!text || text[0] == '\0') {
        showToastError(ui::tr("core.backup_pass_empty"), TOAST_DURATION_MEDIUM_MS);
        return;
    }
    strncpy(s_firstPass, text, PASSPHRASE_MAX);
    s_firstPass[PASSPHRASE_MAX] = '\0';
    pushPassphrase(ui::tr("core.backup_confirm_passphrase"), onExportConfirm);
}

void startExport() {
    wipeFirstPass();
    pushPassphrase(ui::tr("core.backup_passphrase"), onExportFirst);
}

// --- Import ---------------------------------------------------------------

void onImportPass(const char* text) {
    if (!text || text[0] == '\0') {
        showToastError(ui::tr("core.backup_pass_empty"), TOAST_DURATION_MEDIUM_MS);
        return;
    }

    showToastTask(ui::tr("core.task_working"), 0);
    os_ui::BackupSummary s = os_ui::BackupManager::instance().importFrom(text);
    ViewStack::instance().hideModal();

    if (!s.ok) {
        showToastError(ui::tr("core.backup_import_fail"), TOAST_DURATION_LONG_MS);
        return;
    }

    snprintf(s_summaryText, sizeof(s_summaryText),
             "%s\n\n%s: %u\n%s: %u\n%s: %u\n%s: %u\n%s: %s",
             ui::tr("core.backup_scope_info"),
             ui::tr("core.backup_imported"), s.imported,
             ui::tr("core.backup_failed"), s.failed,
             ui::tr("core.backup_modules"), s.modules,
             ui::tr("core.backup_skipped"), s.skipped,
             ui::tr("core.backup_system"),
             ui::tr(s.system ? "core.yes" : "core.no"));
    showInfo(ui::tr("core.backup_summary"), s_summaryText);
}

void startImport() {
    if (!os_ui::BackupManager::instance().backupExists()) {
        showToastInfo(ui::tr("core.backup_none"), TOAST_DURATION_MEDIUM_MS);
        return;
    }
    pushPassphrase(ui::tr("core.backup_passphrase"), onImportPass);
}

// --- Delete ---------------------------------------------------------------

void onDeleteConfirm(void* /*userData*/) {
    bool ok = os_ui::BackupManager::instance().deleteBackup();
    showToast(ok ? ui::tr("core.deleted") : ui::tr("core.failed"),
              TOAST_DURATION_SHORT_MS);
}

void startDelete() {
    if (!os_ui::BackupManager::instance().backupExists()) {
        showToastInfo(ui::tr("core.backup_none"), TOAST_DURATION_MEDIUM_MS);
        return;
    }
    showConfirm(ui::tr("core.backup_delete_q"), onDeleteConfirm, nullptr,
                ConfirmView::Icon::WARNING);
}

// --- Menu -----------------------------------------------------------------

void onBackupMenuSelect(uint16_t index, void* /*userData*/) {
    switch (index) {
        case 0: startExport(); break;
        case 1: startImport(); break;
        case 2: startDelete(); break;
        default: break;
    }
}

// --- Serial: BACKUP EXPORT/IMPORT/DELETE (AUTH-gated, hardcoded English) ---

using cdc::serial::Console;

void cmdBackupExport(const char* args) {
    if (!args || !*args) { Console::printf("Usage: BACKUP EXPORT <passphrase>\r\n"); return; }
    if (os_ui::BackupManager::instance().exportTo(args)) {
        Console::printf("OK: Backup written\r\n");
    } else {
        Console::printf("ERROR: Backup export failed\r\n");
    }
}

void cmdBackupImport(const char* args) {
    if (!args || !*args) { Console::printf("Usage: BACKUP IMPORT <passphrase>\r\n"); return; }
    if (!os_ui::BackupManager::instance().backupExists()) {
        Console::printf("ERROR: No backup present\r\n");
        return;
    }
    os_ui::BackupSummary s = os_ui::BackupManager::instance().importFrom(args);
    if (!s.ok) {
        Console::printf("ERROR: Import failed (wrong passphrase or corrupt file)\r\n");
        return;
    }
    Console::printf("NOTE: FIDO2/GPG signing keys are NOT restored.\r\n");
    Console::printf("OK: imported=%u failed=%u modules=%u skipped=%u system=%u\r\n",
                    s.imported, s.failed, s.modules, s.skipped, s.system ? 1 : 0);
}

void cmdBackupDelete(const char* /*args*/) {
    if (!os_ui::BackupManager::instance().backupExists()) {
        Console::printf("OK: No backup present\r\n");
        return;
    }
    Console::printf(os_ui::BackupManager::instance().deleteBackup()
                        ? "OK: Backup deleted\r\n"
                        : "ERROR: Delete failed\r\n");
}

const cdc::serial::SubCommand kBackupSubs[] = {
    {"EXPORT", "<passphrase>", "Write encrypted backup to vFAT", cmdBackupExport},
    {"IMPORT", "<passphrase>", "Restore from the on-device backup", cmdBackupImport},
    {"DELETE", "",             "Delete the on-device backup",      cmdBackupDelete},
    {nullptr, nullptr, nullptr, nullptr},
};

void cmdBackup(const char* args) {
    cdc::serial::dispatchSubCommand("BACKUP", args, kBackupSubs);
}

} // namespace

/**
 * \brief Shows the Backup submenu (Export / Import / Delete).
 */
void showBackupMenu() {
    if (!s_backupMenu) {
        s_backupMenu = new ListView();
        s_backupMenu->setOnSelect(onBackupMenuSelect);
    }
    s_backupItems[0] = {ui::tr("core.backup_export"), 0, false, nullptr};
    s_backupItems[1] = {ui::tr("core.backup_import"), 0, false, nullptr};
    s_backupItems[2] = {ui::tr("core.backup_delete"), 0, false, nullptr};
    s_backupMenu->init(ui::tr("core.backup"), s_backupItems, 3);
    ViewStack::instance().push(s_backupMenu);
}

/**
 * \brief Registers the AUTH-gated BACKUP serial command.
 */
void registerBackupSerialCommand() {
    cdc::serial::getCommandRegistry().registerCommand(
        {"BACKUP", "Encrypted backup: EXPORT/IMPORT/DELETE",
         cmdBackup, "backup", true, kBackupSubs});
}

} // namespace cdc::ui
