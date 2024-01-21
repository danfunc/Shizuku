#include "shizuku/object.hpp"
using namespace shizuku::types;

void object::export_method(method entry, shizuku::string const &name) {
  method_map.insert_or_assign(name, entry);
}