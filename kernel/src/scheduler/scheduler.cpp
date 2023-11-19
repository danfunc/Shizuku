
#include "shizuku/kernel.hpp"

void shizuku::types::kernel::schedule() {
  for (auto object_iterator = object_tree.begin();
       object_iterator != object_tree.end(); ++object_iterator) {
    for (auto thread_iterator = object_iterator->second->thread_map.begin();
         thread_iterator != object_iterator->second->thread_map.end();
         ++thread_iterator) {
      thread_iterator->context;
    }
  }
}