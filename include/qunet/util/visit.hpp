#pragma once

namespace qn {

template<class... Ts> struct makeVisitor : Ts... { using Ts::operator()...; };
template<class... Ts> makeVisitor(Ts...) -> makeVisitor<Ts...>;

}