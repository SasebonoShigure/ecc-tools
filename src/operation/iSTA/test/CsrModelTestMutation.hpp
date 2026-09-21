#pragma once
#include <utility>

namespace ista::csr::test {
template<class Model, class Mutation>
void mutateChildren(Model& model, Mutation mutate)
{
  auto children = model.children();
  mutate(children);
  model.setChildren(children);
}
}  // namespace ista::csr::test
