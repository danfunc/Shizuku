#include "shizuku/kernel.hpp"

int shizuku::types::kernel::add_thread(int (*entry)(int, char **), int argc,
                                       char **argv) {
  if (auto current_object = this->cpu_manager[cpu_driver::get_core_num()]
                                .get_current_thread()
                                .lock()
                                ->parent_object) {
    shizuku::platform::std::shared_ptr<shizuku::types::thread> new_thread =
        shizuku::platform::std::make_shared<shizuku::types::thread>(
            entry, argc, argv, current_object);
    new_thread->thread_id = ++this->thread_count;
    current_object->threads.insert(new_thread);
    return new_thread->thread_id;
  } else
    return 0;
}
