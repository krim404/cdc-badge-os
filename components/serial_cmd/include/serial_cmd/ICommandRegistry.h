#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::serial {

/**
 * Command handler function type
 * @param args Arguments after command name (trimmed, null-terminated)
 */
using CommandHandler = void (*)(const char* args);

/**
 * Sub-command descriptor.
 *
 * Used to attach a list of sub-commands to a top-level Command. The registry
 * uses this for help output; dispatch is done with dispatchSubCommand() from
 * SubCommand.h, which walks the same table.
 */
struct SubCommand {
    const char* name;      ///< Sub-command keyword, e.g. "LIST". `nullptr` terminates the array.
    const char* args;      ///< Argument hint shown in HELP, e.g. "<ns> <key>". May be "" or nullptr.
    const char* help;      ///< One-line description.
    CommandHandler handler;///< Invoked with the args following the sub-command keyword.
};

/// Buffer size for the "<name> <args>" column built during HELP rendering.
constexpr size_t kSubCommandHeadBufSize = 80;

/**
 * Command entry for registration
 */
struct Command {
    const char* name;                       ///< Top-level command (e.g. "TOTP" or "PING").
    const char* help;                       ///< One-line summary shown in HELP.
    CommandHandler handler;                 ///< Top-level dispatcher / handler.
    const char* moduleName;                 ///< Module that registered the command (used for HELP grouping).
    bool requiresAuth;                      ///< Whether the command needs an authenticated session.
    const SubCommand* subCommands = nullptr;///< Optional null-terminated sub-command table for HELP.
};

/**
 * Command Registry Interface
 *
 * Modules register their commands here. The registry handles:
 * - Command lookup and dispatch
 * - Help text generation
 * - Optional authentication requirements
 */
class ICommandRegistry {
public:
    virtual ~ICommandRegistry() = default;

    /**
     * Register a command
     * @param cmd Command definition
     * @return true if registered successfully
     */
    virtual bool registerCommand(const Command& cmd) = 0;

    /**
     * Unregister all commands from a module
     * @param moduleName Module name used in registration
     */
    virtual void unregisterModule(const char* moduleName) = 0;

    /**
     * Process a command line
     * @param line Full command line (command + arguments)
     * @return true if command was found and executed
     */
    virtual bool processCommand(const char* line) = 0;

    /**
     * Show help for all commands
     */
    virtual void showHelp() = 0;

    /**
     * Get number of registered commands
     */
    virtual size_t getCommandCount() const = 0;

    /**
     * Set authentication provider callback
     * @param authCheck Function that returns true if session is authenticated
     */
    virtual void setAuthProvider(bool (*authCheck)()) = 0;

    /**
     * Set callback for successful command execution
     * Used to reset auth timer when FEATURE_SECURE_SERIAL is enabled
     */
    virtual void setOnCommandExecuted(void (*callback)()) = 0;

    /**
     * Line interceptor for multiline input modes (e.g., vCard paste).
     * When set, called before normal command dispatch.
     * Return true to consume the line, false for normal processing.
     */
    using LineInterceptor = bool (*)(const char* line);

    /**
     * Set or clear the line interceptor
     * @param interceptor Callback, or nullptr to clear
     */
    virtual void setLineInterceptor(LineInterceptor interceptor) { (void)interceptor; }

    /**
     * Byte interceptor for binary streaming modes (e.g. raw plugin upload).
     *
     * While installed, every byte received from serial is passed directly to
     * the handler instead of being echoed, accumulated in the line buffer or
     * dispatched as a command. The interceptor decides when to remove itself
     * (typically once the expected payload size has been received) by
     * calling `setByteInterceptor(nullptr)`.
     */
    using ByteInterceptor = void (*)(uint8_t byte);

    /**
     * Set or clear the byte interceptor. While set, line-based parsing is
     * fully bypassed.
     */
    virtual void setByteInterceptor(ByteInterceptor interceptor) { (void)interceptor; }
    virtual ByteInterceptor getByteInterceptor() const { return nullptr; }
};

// Get global command registry instance
ICommandRegistry& getCommandRegistry();

} // namespace cdc::serial
