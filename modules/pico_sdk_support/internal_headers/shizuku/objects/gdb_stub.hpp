#ifndef SHIZUKU_OBJECTS_GDB_STUB_HPP
#define SHIZUKU_OBJECTS_GDB_STUB_HPP
#include <cstdint>
#include "shizuku/object_ids.hpp"

// ===========================================================================
//  GDB stub — プローブ無しで止めて覗く (docs D40)
// ===========================================================================
//  ★halting debug (SWD) はコアごと止めるが、こちらは DebugMonitor なので
//    **止まるのは対象のスレッドだけ**。他は走り続ける (I-9)。
//  ★入出力は今 USB CDC を直に叩いているが、本来はストリーム経由にすべき。
//    そうなれば診断出力と GDB は 2 本のストリームになり、「診断を黙らせる旗」も
//    要らなくなる (gdb_stub.cpp の read_byte/write_byte だけの差し替えで済む)。
namespace shizuku {
namespace objects {

constexpr uintptr_t GDB_STUB_OBJECT = object_id::gdb_stub;
// 覗かれる側。★デバッグのために特別なことは何もしていない普通のオブジェクト。
constexpr uintptr_t DEBUGGEE_OBJECT = object_id::debuggee;

// 覗かれる側 (debuggee) と stub を起こす。0 = 成功。
uint32_t start_gdb_stub();

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_GDB_STUB_HPP
