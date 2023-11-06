#include "shizuku.hpp"
#include "shizuku/processors/rp2040.hpp"

void shizuku::types::processors::rp2040::cpu_driver::context_switch(
    context &current, context &next) {
  if (save_context(current) == 0) {
    load_context(1, next);
  }
  return;
}

void shizuku::types::processors::rp2040::cpu_driver::add_context(
    context &context) {
  this->context_list.push_front(context);
}

void shizuku::types::processors::rp2040::cpu_driver::context_switch() {
  this->current;
  auto from = current;
  current++;
  auto to = current;
  context_switch(*from, *to);
}