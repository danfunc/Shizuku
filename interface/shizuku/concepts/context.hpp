#ifndef SHIZUKU_TEMPLATE_CONTEXT_HPP
#define SHIZUKU_TEMPLATE_CONTEXT_HPP
namespace shizuku{
namespace concepts {
    template<typename CONTEXT>
    concept context_requires = requires(CONTEXT context){
        true;
    };
    }
} //
#endif // SHIZUKU_TEMPLATE_CONTEXT_HPP