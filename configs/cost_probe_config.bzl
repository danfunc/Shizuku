"""Generate config.hpp with RP2350-only diagnostic build settings."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _config_impl(ctx):
    substitutions = dict(ctx.attr.substitutions)
    substitutions["${SHIZUKU_XIP_COST_PROBE}"] = "1" if ctx.attr.is_rp2350 and ctx.attr.xip_probe[BuildSettingInfo].value else "0"
    substitutions["${SHIZUKU_ROUTE_COST_PROBE}"] = "1" if ctx.attr.is_rp2350 and ctx.attr.route_probe[BuildSettingInfo].value else "0"
    ctx.actions.expand_template(
        template = ctx.file.template,
        output = ctx.outputs.out,
        substitutions = substitutions,
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

cost_probe_config = rule(
    implementation = _config_impl,
    attrs = {
        "template": attr.label(allow_single_file = True, mandatory = True),
        "out": attr.output(mandatory = True),
        "substitutions": attr.string_dict(mandatory = True),
        "is_rp2350": attr.bool(),
        "xip_probe": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "route_probe": attr.label(mandatory = True, providers = [BuildSettingInfo]),
    },
)
