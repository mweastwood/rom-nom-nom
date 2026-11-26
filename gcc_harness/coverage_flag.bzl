"""Native Starlark boolean build setting flag (zero external dependencies).

Provides `bool_flag` for `--//gcc_harness:coverage=true` and an
`uninstrumented_filegroup` outgoing transition rule that resets
`//gcc_harness:coverage` back to `False` for `shared_corpus_traces` so cached
trace shards always stay uninstrumented and hit Bazel's action cache with zero
regenerations even when building instrumented coverage binaries.
"""

BuildSettingInfo = provider(fields = ["value"])

def _bool_flag_impl(ctx):
    return BuildSettingInfo(value = ctx.build_setting_value)

bool_flag = rule(
    implementation = _bool_flag_impl,
    build_setting = config.bool(flag = True),
)

def _disable_coverage_transition_impl(_settings, _attr):
    return {"//gcc_harness:coverage": False}

_disable_coverage_transition = transition(
    implementation = _disable_coverage_transition_impl,
    inputs = [],
    outputs = ["//gcc_harness:coverage"],
)

def _uninstrumented_filegroup_impl(ctx):
    files = depset(transitive = [src.files for src in ctx.attr.srcs])
    return [DefaultInfo(
        files = files,
        runfiles = ctx.runfiles(transitive_files = files),
    )]

uninstrumented_filegroup = rule(
    implementation = _uninstrumented_filegroup_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
            cfg = _disable_coverage_transition,
        ),
    },
)
