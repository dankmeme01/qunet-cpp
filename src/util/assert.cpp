#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <fmt/format.h>
#include <stdexcept>

void qn::_assertionFail(std::string_view what, std::string_view file, int line) {
    qn::log::error("Assertion failed ({} at {}:{})", what, file, line);

    throw std::runtime_error(fmt::format("Assertion failed ({}) at {}:{}", what, file, line));
}
