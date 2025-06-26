#include <qunet/util/assert.hpp>
#include <fmt/format.h>
#include <stdexcept>

void qn::_assertionFail(std::string_view what, std::string_view file, int line) {
    throw std::runtime_error(fmt::format("Assertion failed ({}) at {}:{}", what, file, line));
}
