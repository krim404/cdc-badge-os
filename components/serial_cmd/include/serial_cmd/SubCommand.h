#pragma once

#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/Console.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cdc::serial {

/**
 * \brief Splits args into "<sub-command> <rest>".
 * \param args Raw argument string passed to the dispatcher.
 * \param name Output: pointer to first character of the sub-command keyword.
 * \param name_len Output: length of the keyword (0 if none present).
 * \param rest Output: pointer to the remaining args (whitespace skipped).
 */
inline void parseSubCommand(const char* args,
                            const char*& name,
                            size_t&      name_len,
                            const char*& rest)
{
    while (*args == ' ' || *args == '\t') ++args;
    name = args;
    while (*args && !std::isspace(static_cast<unsigned char>(*args))) ++args;
    name_len = static_cast<size_t>(args - name);
    while (*args == ' ' || *args == '\t') ++args;
    rest = args;
}

/**
 * \brief Case-insensitive compare of an unterminated token against a word.
 */
inline bool subCommandEquals(const char* token, size_t token_len, const char* word)
{
    return std::strlen(word) == token_len &&
           strncasecmp(token, word, token_len) == 0;
}

/**
 * \brief Prints a sub-command summary block.
 * \param parent Top-level command name (e.g. "NVS").
 * \param table Null-terminated SubCommand array.
 */
inline void printSubCommandHelp(const char* parent, const SubCommand* table)
{
    if (!table) return;
    Console::printf("Usage: %s <subcommand> [args]\r\n", parent);
    Console::printf("Subcommands:\r\n");
    for (const SubCommand* e = table; e->name; ++e) {
        char head[kSubCommandHeadBufSize];
        if (e->args && *e->args) {
            std::snprintf(head, sizeof(head), "%s %s", e->name, e->args);
        } else {
            std::snprintf(head, sizeof(head), "%s", e->name);
        }
        Console::printf("  %-26s %s\r\n", head, e->help ? e->help : "");
    }
}

/**
 * \brief Routes a sub-command line to its handler.
 *
 * Empty args or the keyword "HELP" trigger the auto-generated help screen.
 * Unknown keywords print an error followed by the same help screen.
 *
 * \param parent Parent command name, only used for help/error output.
 * \param args Arguments received by the top-level handler.
 * \param table Null-terminated SubCommand array.
 */
inline void dispatchSubCommand(const char* parent,
                               const char* args,
                               const SubCommand* table)
{
    const char* sub = nullptr;
    size_t sub_len = 0;
    const char* rest = nullptr;
    parseSubCommand(args ? args : "", sub, sub_len, rest);

    if (sub_len == 0 || subCommandEquals(sub, sub_len, "HELP") ||
        subCommandEquals(sub, sub_len, "?")) {
        printSubCommandHelp(parent, table);
        return;
    }

    for (const SubCommand* e = table; e->name; ++e) {
        if (subCommandEquals(sub, sub_len, e->name)) {
            if (e->handler) e->handler(rest);
            return;
        }
    }

    Console::printf("ERROR: Unknown %s subcommand '%.*s'\r\n",
                    parent, static_cast<int>(sub_len), sub);
    printSubCommandHelp(parent, table);
}

} // namespace cdc::serial
