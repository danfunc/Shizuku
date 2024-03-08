#include "shizuku/object.hpp"

using namespace shizuku::types;

void object::remove_thread(size_t thread_id) { thread_map.erase(thread_id); }