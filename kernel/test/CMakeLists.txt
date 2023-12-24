target_sources(shizuku_kernel PRIVATE MMU_emulate_test.cpp multithread_test.cpp
                                      object_system_test.cpp)
set(PICO_COPY_TO_RAM=1)
