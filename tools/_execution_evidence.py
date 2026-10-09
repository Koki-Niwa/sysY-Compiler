"""S11 execution evidence: immutable identities and probed target facts."""
import hashlib
import os
import shutil
import subprocess

def sha256_file(path):
    if not path or not os.path.isfile(path):
        return None
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def probe(command):
    result = subprocess.run(command, capture_output=True, timeout=15)
    rc, out, err, timed_out = (result.returncode, result.stdout,
                              result.stderr.decode("utf-8", "replace"), False)
    if rc != 0 or timed_out:
        raise RuntimeError("toolchain probe failed: %r: %s" % (command, err))
    return out.decode("utf-8", "replace").strip()


def executable(command):
    path = shutil.which(command) if not os.path.isabs(command) else command
    if not path:
        raise RuntimeError("required executable missing: " + command)
    return os.path.realpath(path)


def evidence(args, TARGETS):
    target = TARGETS[args.target]
    llvm_as = executable(os.path.join(args.llvm_bin, "llvm-as"))
    llc = executable(os.path.join(args.llvm_bin, "llc"))
    linker = executable(target["link_cc"])
    runner = executable(target["runner"][0]) if target["runner"] else executable("uname")
    revision = probe(["git", "-C", os.path.join(os.path.dirname(__file__), ".."), "rev-parse", "HEAD"])
    dirty = bool(probe(["git", "-C", os.path.join(os.path.dirname(__file__), ".."), "status", "--porcelain"]))
    config = {"triple": target["llc_triple"], "llc_flags": target["llc_extra"],
              "link_flags": (["-static"] if target["static"] else []) + ["-w", "-lm"] + target["link_extra"]}
    facts = {"dumpmachine": probe([linker, "-dumpmachine"])}
    if args.target == "riscv64":
        options = probe([linker, "-Q", "--help=target"])
        import re
        for flag in ("march", "mabi"):
            found = re.search(r"(?m)^\s*-" + flag + r"=\s*(\S+)", options)
            if not found:
                raise RuntimeError("cannot probe RISC-V " + flag)
            facts[flag] = found.group(1)
    config["probed"] = facts
    return {
        "schema_version": 1,
        "compiler": {"revision": revision, "dirty": dirty, "executable": args.compiler,
                     "executable_sha256": sha256_file(args.compiler)},
        "manifest": {"path": os.path.realpath(args.manifest),
                     "sha256": sha256_file(args.manifest)},
        "mode": {"target": args.target, "optimize": args.optimize,
                 "no_structured": args.no_structured,
                 "boundary": "from-flat" if args.no_structured else "source"},
        "toolchain": {
            "llvm": {"llvm_as": {"path": llvm_as, "version": probe([llvm_as, "--version"])},
                     "llc": {"path": llc, "version": probe([llc, "--version"])}},
            "linker": {"path": linker, "version": probe([linker, "--version"]).splitlines()[0],
                       "dumpmachine": facts["dumpmachine"]},
            "execution": {"kind": "qemu" if target["runner"] else "native",
                          "runner": runner, "runner_version": probe([runner, "--version"]).splitlines()[0]
                          if target["runner"] else probe([runner, "-r"]),
                          "real_target_performance": not bool(target["runner"]),
                          "not_real_target_performance": bool(target["runner"])},
            "target_config": config
        },
        "measurement": {"peak_rss": "unavailable",
                        "qemu_timing": "proxy diagnostic" if target["runner"] else "native"}
    }
