#pragma once

#include <string_view>

namespace qn {

std::string_view messageForConnectionError(int code);
std::string_view messageForServerCloseError(int code);
std::string_view messageForHandshakeError(int code);

}