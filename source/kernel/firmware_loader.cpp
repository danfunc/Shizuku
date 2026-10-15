#include "shizuku/firmware_loader.hpp"
#include "shizuku/object_api.hpp"

namespace {
// Maximum composition objects supported in the interim root-only loader.
// Manifests exceeding this bound are rejected fail-closed; never silently truncated.
constexpr uint32_t SHIZUKU_LOADER_MAX_OBJECTS = 8;
} // namespace

extern "C" bool shizuku_firmware_load_and_start(const shizuku_object_manifest_entry_t *manifest, uint32_t count) {
    // Fail-closed validation: Reject null, zero, or oversized manifests exceeding capacity limit
    if (manifest == nullptr || count == 0 || count > SHIZUKU_LOADER_MAX_OBJECTS) {
        return false;
    }

    // 1. 各オブジェクトを本番 CREATE_OBJECT 経路で作成 & 検証
    for (uint32_t i = 0; i < count; ++i) {
        const auto &item = manifest[i];
        if (item.id < 2 || item.entry == nullptr) {
            return false;
        }

        // Authority validation: manifest parent_id represents object authority hierarchy.
        // Currently, canonical trusted root is ID 1. Non-root parent delegation is not yet
        // supported in the composition loader; fail-closed rather than silently ignoring.
        if (item.parent_id != 1) {
            return false;
        }

        // 明示的な OBJECT_PRIVILEGED (1<<0) / OBJECT_UNPRIVILEGED (1<<1) フラグ設定
        uintptr_t flags = item.privileged ? shizuku::OBJECT_PRIVILEGED : shizuku::OBJECT_UNPRIVILEGED;

        auto result = shizuku_loader_seam_syscall(
            static_cast<uintptr_t>(shizuku::object_api::CREATE_OBJECT),
            item.id,
            reinterpret_cast<uintptr_t>(item.entry),
            flags
        );

        // CREATE_OBJECT 失敗時は即座に中断
        if (result.error != 0) {
            return false;
        }
    }

    // 2. 各オブジェクトのエントリスレッドを本番 object_api::SPAWN 経路で起動 & 検証
    // object_api::SPAWN (syscall index 7) は KERNEL_OBJECT::spawn_method を実行し、
    // スタック・台帳の割当、親ハンドラ・種別・保護・アフィニティの束縛を完結する。
    // 引数: a1 = object ID, a2 = method index (0 = main), a3 = entry argument (0)
    for (uint32_t i = 0; i < count; ++i) {
        const auto &item = manifest[i];

        auto result = shizuku_loader_seam_syscall(
            static_cast<uintptr_t>(shizuku::object_api::SPAWN),
            item.id,
            0, // method index 0 (main method registered during CREATE_OBJECT)
            0  // entry argument
        );

        // SPAWN 失敗時は即座に中断 (メモリ不足・スレッド上限等)
        if (result.error != 0) {
            return false;
        }
    }

    return true;
}
