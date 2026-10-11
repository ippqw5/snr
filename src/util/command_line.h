#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace snr
{
// A small option registry. Every option is registered once, and the registry renders the usage
// text from the same list, so an option cannot be added without also being documented.
class CommandLine
{
public:
    CommandLine(int argc, char** argv, std::string title);

    // A flag takes no value and sets its target when present.
    void flag(const std::string& name, const std::string& help, bool& target);
    // A flag whose effect is not a plain boolean assignment.
    void flag(const std::string& name, const std::string& help, std::function<void()> apply);

    // An integer option with an inclusive range. `value` names the argument in the usage text.
    void integer(
        const std::string& name,
        const std::string& value,
        const std::string& help,
        uint32_t&          target,
        uint32_t           minimum,
        uint32_t           maximum
    );
    // A floating-point option with an inclusive range.
    void real(
        const std::string& name,
        const std::string& value,
        const std::string& help,
        float&             target,
        float              minimum,
        float              maximum
    );
    // Assigns the next argument; works for std::string and std::filesystem::path targets.
    template <typename Target>
    void text(const std::string& name, const std::string& value, const std::string& help, Target& target)
    {
        add(name, value, help, [this, &target](int& index) { target = next(index); });
    }
    // For options that read several values or write more than one field.
    void custom(
        const std::string&                            name,
        const std::string&                            value,
        const std::string&                            help,
        std::function<void(CommandLine&, int& index)> apply
    );

    // Matches argv against the registered options. Throws std::runtime_error on an unknown option
    // or a value that is missing or out of range.
    void parse();
    // The usage text, generated from the registered options.
    std::string usage() const;
    // Whether --help was seen. --help is registered automatically, so it always exists.
    bool helpRequested() const;

    // Value helpers for custom() handlers.
    std::string next(int& index);
    uint32_t    nextInteger(int& index, uint32_t minimum, uint32_t maximum);
    float       nextReal(int& index, float minimum, float maximum);

private:
    struct Option
    {
        std::string               name;
        std::string               value;
        std::string               help;
        std::function<void(int&)> apply;
        bool                      trailing = false;

        std::string signature() const;
    };

    void add(
        const std::string&        name,
        const std::string&        value,
        const std::string&        help,
        std::function<void(int&)> apply,
        bool                      trailing = false
    );

    int                 m_argc;
    char**              m_argv;
    std::string         m_title;
    std::string         m_program;
    bool                m_help = false;
    std::vector<Option> m_options;
};
} // namespace snr
