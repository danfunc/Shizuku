#ifndef SHIZUKU_TEMPLATE_OBJECT_HPP
#define SHIZUKU_TEMPLATE_OBJECT_HPP
namespace shizuku
{
    namespace templates
    {
        template<typename THREAD_TABLE,typename METHOD_TABLE,typename MEMORY_TABLE>
        struct object
        {
            THREAD_TABLE thread_table;
            METHOD_TABLE method_table;
            MEMORY_TABLE memory_table;
        };
        
    } // namespace templates
    
} // namespace shizuku

#endif // SHIZUKU_TEMPLATE_OBJECT_HPP