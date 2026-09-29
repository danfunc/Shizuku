load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _flash_layout_config_impl(ctx):
    capacity = ctx.attr.capacity[BuildSettingInfo].value
    output = ctx.actions.declare_file("internal_headers/shizuku/objects/generated_flash_layout_config.hpp")
    ctx.actions.write(
        output = output,
        content = """// Generated from //configs:flash_capacity_bytes.
#ifndef SHIZUKU_GENERATED_FLASH_LAYOUT_CONFIG_HPP
#define SHIZUKU_GENERATED_FLASH_LAYOUT_CONFIG_HPP
#define SHIZUKU_FLASH_CAPACITY_BYTES %d
#endif
""" % capacity,
    )
    return [DefaultInfo(files = depset([output]))]

flash_layout_config = rule(
    implementation = _flash_layout_config_impl,
    attrs = {
        "capacity": attr.label(
            default = "//configs:flash_capacity_bytes",
            providers = [BuildSettingInfo],
        ),
    },
    doc = "Generates the configured logical flash capacity header.",
)
