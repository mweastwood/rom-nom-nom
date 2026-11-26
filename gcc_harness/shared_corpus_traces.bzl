"""Starlark macro generating Bazel-cached stride-sharded forward trace files."""

load(":coverage_flag.bzl", "uninstrumented_filegroup")

def shared_corpus_trace_shards(name = "shared_corpus_traces", num_shards = 32):
    """Defines `num_shards` parallel testonly genrules and an aggregating filegroup.

    Each shard `i` compiles all shared corpus cases with `k % num_shards == i`
    through in-process KMC GCC 2.7.2 + GAS 2.6 once and caches the lossless
    deterministic binary archive under Bazel's action cache.
    """
    shard_targets = []
    for i in range(num_shards):
        suffix = ("0" + str(i)) if i < 10 else str(i)
        rule_name = "%s_shard_%s" % (name, suffix)
        out_file = "%s_shard_%s.bin" % (name, suffix)
        native.genrule(
            name = rule_name,
            outs = [out_file],
            cmd = "$(location :shared_corpus_trace_generator) --shard_index=%d --num_shards=%d --output=$@" % (
                i,
                num_shards,
            ),
            testonly = True,
            tools = [":shared_corpus_trace_generator"],
        )
        shard_targets.append(":" + rule_name)

    uninstrumented_filegroup(
        name = name,
        srcs = shard_targets,
        testonly = True,
    )
