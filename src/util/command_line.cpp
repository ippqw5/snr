#include "util/command_line.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace snr
{
CommandLine::CommandLine(int argc, char** argv, std::string title)
    : m_argc(argc)
    , m_argv(argv)
    , m_title(std::move(title))
    , m_program(argc > 0 ? std::filesystem::path(argv[0]).stem().string() : std::string("pathtracer"))
{
    // Registered here rather than by the caller so help can never be forgotten, and marked
    // trailing so the usage text lists it last.
    add("--help", {}, "Print this help", [this](int&) { m_help = true; }, true);
}

void CommandLine::flag(const std::string& name, const std::string& help, bool& target)
{
    add(name, {}, help, [&target](int&) { target = true; });
}

void CommandLine::flag(const std::string& name, const std::string& help, std::function<void()> apply)
{
    add(name, {}, help, [apply = std::move(apply)](int&) { apply(); });
}

void CommandLine::integer(
    const std::string& name,
    const std::string& value,
    const std::string& help,
    uint32_t&          target,
    uint32_t           minimum,
    uint32_t           maximum
)
{
    add(name, value, help, [this, &target, minimum, maximum](int& index) {
        target = nextInteger(index, minimum, maximum);
    });
}

void CommandLine::real(
    const std::string& name,
    const std::string& value,
    const std::string& help,
    float&             target,
    float              minimum,
    float              maximum
)
{
    add(name, value, help, [this, &target, minimum, maximum](int& index) {
        target = nextReal(index, minimum, maximum);
    });
}

void CommandLine::custom(
    const std::string&                            name,
    const std::string&                            value,
    const std::string&                            help,
    std::function<void(CommandLine&, int& index)> apply
)
{
    add(name, value, help, [this, apply = std::move(apply)](int& index) { apply(*this, index); });
}

void CommandLine::parse()
{
    for (int index = 1; index < m_argc; ++index)
    {
        const std::string name = m_argv[index];
        const auto        found = std::find_if(
            m_options.begin(),
            m_options.end(),
            [&](const Option& option) { return option.name == name; }
        );
        if (found == m_options.end())
            throw std::runtime_error("Unknown option: " + name);

        found->apply(index);
    }
}

std::string CommandLine::usage() const
{
    size_t width = 0;
    for (const Option& option : m_options)
        width = std::max(width, option.signature().size());

    std::string text = m_title + "\nUsage: " + m_program + " [options]\n";
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const Option& option : m_options)
        {
            if (option.trailing != (pass == 1))
                continue;

            const std::string signature = option.signature();
            text += "  " + signature + std::string(width - signature.size(), ' ') + "  " + option.help + "\n";
        }
    }
    return text;
}

bool CommandLine::helpRequested() const
{
    return m_help;
}

std::string CommandLine::next(int& index)
{
    if (++index >= m_argc)
        throw std::runtime_error("Missing value for " + std::string(m_argv[index - 1]));
    return m_argv[index];
}

uint32_t CommandLine::nextInteger(int& index, uint32_t minimum, uint32_t maximum)
{
    const std::string value = next(index);
    uint32_t          result = 0;
    const auto        parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || result < minimum || result > maximum)
        throw std::runtime_error("Integer option out of range: " + value);
    return result;
}

float CommandLine::nextReal(int& index, float minimum, float maximum)
{
    const std::string value = next(index);
    char*             end = nullptr;
    const float       result = std::strtof(value.c_str(), &end);
    if (end != value.c_str() + value.size() || end == value.c_str() || !std::isfinite(result) || result < minimum || result > maximum)
        throw std::runtime_error("Floating-point option out of range: " + value);
    return result;
}

std::string CommandLine::Option::signature() const
{
    return value.empty() ? name : name + " " + value;
}

void CommandLine::add(
    const std::string&        name,
    const std::string&        value,
    const std::string&        help,
    std::function<void(int&)> apply,
    bool                      trailing
)
{
    m_options.push_back({name, value, help, std::move(apply), trailing});
}
} // namespace snr
