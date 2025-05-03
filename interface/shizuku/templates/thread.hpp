#ifndef SHIZUKU_TEMPLATE_THREAD_HPP
#define SHIZUKU_TEMPLATE_THREAD_HPP
namespace shizuku{
    namespace templates{
        template <typename CONTEXT>
        struct thread{
            CONTEXT context;
            thread(CONTEXT context) : context(context) {
                
            }

        }
    }
} //
#endif // SHIZUKU_TEMPLATE_THREAD_HPP