#include "Lib.hh"

#include <stdexcept>
#include <utility>

namespace {
void require(bool condition)
{
  if (!condition)
    throw std::runtime_error("Liberty expression ownership changed");
}

LibertyExpr* expression(const char* text)
{
  auto* result = liberty_convert_expr(liberty_parse_expr(text));
  require(result != nullptr);
  return result;
}
}  // namespace

int main()
{
  idb::LibPort source("Y");
  source.set_ower_cell(nullptr);
  auto* first = expression("A & !B");
  source.set_func_expr(first);
  source.set_func_expr_str("A & !B");
  idb::LibPort moved(std::move(source));
  require(source.get_func_expr() == nullptr && moved.get_func_expr() == first);
  require(moved.get_func_expr_str() == "A & !B");

  idb::LibPort target("Z");
  target.set_func_expr(expression("C"));
  target = std::move(moved);
  require(moved.get_func_expr() == nullptr && target.get_func_expr() == first);
  require(target.get_func_expr_str() == "A & !B");
  auto* same = &target;
  target = std::move(*same);
  require(target.get_func_expr() == first);

  auto* replacement = expression("D | E");
  target.set_func_expr(replacement);
  require(target.get_func_expr() == replacement);
  target.set_func_expr(nullptr);
  require(target.get_func_expr() == nullptr);
  target.set_func_expr(expression("F"));
  return 0;
}
