/**
 * Command Registry Implementation
 * Manages registered commands and dispatches to handlers
 */

#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/Console.h"
#include "cdc_core/feature_flags.h"
#include "cdc_core/PinManager.h"
#include "cdc_log.h"
#include <cstdio>
#include <cstring>
#include <cctype>

static const char* TAG = "CMDREGS";

namespace cdc::serial {

/**
 * \brief Maximum number of commands that can be registered.
 */
static constexpr size_t MAX_COMMANDS = 64;

class CommandRegistry : public ICommandRegistry {
public:
    /**
     * \brief Sets external authentication status provider.
     * \param authCheck Callback returning current authentication state.
     * \return void
     */
    void setAuthProvider(bool (*authCheck)()) override {
        authCheck_ = authCheck;
    }

    /**
     * \brief Registers a command in the dispatch table.
     * \param cmd Command descriptor.
     * \return `true` if registration succeeded.
     */
    bool registerCommand(const Command& cmd) override {
        if (count_ >= MAX_COMMANDS) {
            LOG_W(TAG, "Command limit reached");
            return false;
        }

        // Check for duplicate
        for (size_t i = 0; i < count_; i++) {
            if (strcasecmp(commands_[i].name, cmd.name) == 0) {
                LOG_W(TAG, "Command '%s' already registered", cmd.name);
                return false;
            }
        }

        commands_[count_++] = cmd;
        LOG_I(TAG, "Registered command: %s (%s)", cmd.name, cmd.moduleName);
        return true;
    }

    /**
     * \brief Unregisters all commands belonging to one module.
     * \param moduleName Module name key.
     * \return void
     */
    void unregisterModule(const char* moduleName) override {
        if (!moduleName) return;

        size_t writeIdx = 0;
        for (size_t readIdx = 0; readIdx < count_; readIdx++) {
            if (commands_[readIdx].moduleName &&
                strcmp(commands_[readIdx].moduleName, moduleName) == 0) {
                // Skip this command (remove it)
                continue;
            }
            if (writeIdx != readIdx) {
                commands_[writeIdx] = commands_[readIdx];
            }
            writeIdx++;
        }
        count_ = writeIdx;
    }

    /**
     * \brief Sets optional line interceptor for multiline modes.
     * \param interceptor Interceptor callback.
     * \return void
     */
    void setLineInterceptor(LineInterceptor interceptor) override {
        lineInterceptor_ = interceptor;
    }

    void setByteInterceptor(ByteInterceptor interceptor) override {
        byteInterceptor_ = interceptor;
    }

    ByteInterceptor getByteInterceptor() const override {
        return byteInterceptor_;
    }

    /**
     * \brief Parses and executes one command line.
     * \param line Raw command line.
     * \return `true` if line was handled by registry/interceptor.
     */
    bool processCommand(const char* line) override {
        if (!line || !*line) return false;

        // Check line interceptor first (multiline input modes)
        if (lineInterceptor_ && lineInterceptor_(line)) {
            return true;
        }

        // Find command name (first word)
        char cmdBuf[64];
        size_t cmdLen = 0;
        while (*line && !isspace(*line) && cmdLen < sizeof(cmdBuf) - 1) {
            cmdBuf[cmdLen++] = *line++;
        }
        cmdBuf[cmdLen] = '\0';

        // Skip whitespace to get to arguments
        while (*line && isspace(*line)) line++;

#if FEATURE_SECURE_SERIAL
        // Check if PIN is blocked (lockout or retries exhausted)
        // When blocked, only PING is allowed (to check device is alive)
        auto& pm = cdc::core::PinManager::instance();
        if (pm.isBadgeBlocked()) {
            if (strcasecmp(cmdBuf, "PING") != 0) {
                if (pm.isLockoutActive()) {
                    uint32_t remainingSec = pm.getLockoutRemainingMs() / 1000;
                    Console::printf("ERROR: PIN locked. Wait %lu seconds.\r\n",
                                   (unsigned long)remainingSec);
                } else {
                    Console::printf("ERROR: PIN permanently locked.\r\n");
                }
                return true;  // Command blocked
            }
            // PING is allowed even when blocked
        } else {
            // When secure serial is enabled, block ALL commands except PING and AUTH
            // when not authenticated
            bool isAllowedWithoutAuth = (strcasecmp(cmdBuf, "PING") == 0 ||
                                          strcasecmp(cmdBuf, "AUTH") == 0);
            if (!isAllowedWithoutAuth && authCheck_ && !authCheck_()) {
                Console::printf("ERROR: Not authenticated. Use AUTH <pin> to login.\r\n");
                return true;  // Command blocked
            }
        }
#endif

        // Find and execute command
        for (size_t i = 0; i < count_; i++) {
            if (strcasecmp(commands_[i].name, cmdBuf) == 0) {
                // Per-command auth check (for commands that require auth even when
                // FEATURE_SECURE_SERIAL is disabled)
                if (commands_[i].requiresAuth && authCheck_ && !authCheck_()) {
                    Console::printf("ERROR: Authentication required. Use AUTH <pin> first.\r\n");
                    return true;  // Command found but not executed
                }
                if (commands_[i].handler) {
                    commands_[i].handler(line);
                }
                // Signal successful command execution for timer reset
                if (onCommandExecuted_) {
                    onCommandExecuted_();
                }
                return true;
            }
        }

        Console::printf("ERROR: Unknown command '%s'\r\n", cmdBuf);
        Console::printf("Type 'HELP' for available commands.\r\n");
        return false;
    }

    /**
     * \brief Prints grouped help for all registered commands.
     * \return void
     */
    void showHelp() override {
        Console::printf("=== Available Commands ===\r\n");

        // Group by module
        const char* currentModule = nullptr;

        for (size_t i = 0; i < count_; i++) {
            const char* module = commands_[i].moduleName ? commands_[i].moduleName : "system";

            if (!currentModule || strcmp(currentModule, module) != 0) {
                Console::printf("\r\n[%s]\r\n", module);
                currentModule = module;
            }

            Console::printf("  %-20s %s\r\n",
                           commands_[i].name,
                           commands_[i].help ? commands_[i].help : "");

            if (!commands_[i].subCommands) continue;
            for (const SubCommand* e = commands_[i].subCommands; e->name; ++e) {
                char head[40];
                if (e->args && *e->args) {
                    std::snprintf(head, sizeof(head), "%s %s", e->name, e->args);
                } else {
                    std::snprintf(head, sizeof(head), "%s", e->name);
                }
                Console::printf("    %-22s %s\r\n",
                               head, e->help ? e->help : "");
            }
        }

        Console::printf("\r\n");
        Console::flush();
    }

    /**
     * \brief Returns count of currently registered commands.
     * \return Number of registered commands.
     */
    size_t getCommandCount() const override {
        return count_;
    }

    /**
     * \brief Sets callback fired after successful command execution.
     * \param callback Completion callback.
     * \return void
     */
    void setOnCommandExecuted(void (*callback)()) override {
        onCommandExecuted_ = callback;
    }

private:
    Command commands_[MAX_COMMANDS] = {};
    size_t count_ = 0;
    bool (*authCheck_)() = nullptr;
    void (*onCommandExecuted_)() = nullptr;
    LineInterceptor lineInterceptor_ = nullptr;
    ByteInterceptor byteInterceptor_ = nullptr;
};

/**
 * \brief Returns singleton command-registry interface.
 * \return Reference to global `ICommandRegistry` implementation.
 */
ICommandRegistry& getCommandRegistry() {
    static CommandRegistry* g_commandRegistry = new CommandRegistry();
    return *g_commandRegistry;
}

} // namespace cdc::serial
