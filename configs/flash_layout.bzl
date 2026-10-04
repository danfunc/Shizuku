load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _flash_layout_config_impl(ctx):
    capacity = ctx.attr.capacity[BuildSettingInfo].value
    firmware_bytes = ctx.attr.firmware_bytes
    staging_bytes = ctx.attr.staging_bytes
    fs_bytes = ctx.attr.fs_bytes

    lines = [
        "// Generated from //configs:flash_layout_config.",
        "#ifndef SHIZUKU_GENERATED_FLASH_LAYOUT_CONFIG_HPP",
        "#define SHIZUKU_GENERATED_FLASH_LAYOUT_CONFIG_HPP",
        "#define SHIZUKU_FLASH_CAPACITY_BYTES %d" % capacity,
    ]
    if firmware_bytes > 0:
        lines.append("#define SHIZUKU_FIRMWARE_BYTES %d" % firmware_bytes)
    if staging_bytes > 0:
        lines.append("#define SHIZUKU_STAGING_BYTES %d" % staging_bytes)
    if fs_bytes > 0:
        lines.append("#define SHIZUKU_FS_BYTES %d" % fs_bytes)
    lines.append("#endif\n")

    output = ctx.actions.declare_file("internal_headers/shizuku/objects/generated_flash_layout_config.hpp")
    ctx.actions.write(
        output = output,
        content = "\n".join(lines),
    )
    return [DefaultInfo(files = depset([output]))]

flash_layout_config = rule(
    implementation = _flash_layout_config_impl,
    attrs = {
        "capacity": attr.label(
            default = "//configs:flash_capacity_bytes",
            providers = [BuildSettingInfo],
        ),
        "firmware_bytes": attr.int(
            default = 0,
            doc = "Configured firmware partition ceiling in bytes.",
        ),
        "staging_bytes": attr.int(
            default = 0,
            doc = "Configured OTA staging partition size in bytes (or 0 for remainder).",
        ),
        "fs_bytes": attr.int(
            default = 0,
            doc = "Configured flash FS partition size in bytes (or 0 for remainder).",
        ),
    },
    doc = "Generates the configured logical flash capacity header.",
)
