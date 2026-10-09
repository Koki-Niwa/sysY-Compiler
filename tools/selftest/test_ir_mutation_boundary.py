#!/usr/bin/env python3
"""Negative and positive controls for the F01 optimizer mutation boundary."""
from pathlib import Path
from tempfile import TemporaryDirectory
import importlib.util
import subprocess
import sys

CHECK = Path(__file__).with_name("check_ir_mutation_boundary.py")


def run(root):
    return subprocess.run([sys.executable, str(CHECK), "--root", str(root)],
                          capture_output=True, text=True)


def main():
    forbidden = (
        "i->setOperand(0, v);",
        "i->clearOperands();",
        "i->addSucc(b);",
        "i->setSucc(0, b);",
        "i->clearSuccs();",
        "b->addInst(i);",
        "b->insertInst(0, i);",
        "b->removeInst(i);",
        "m.rebuildCFG();",
        "m.rebuildUseDef();",
        "v->replaceAllUsesWith(w);",
    )
    with TemporaryDirectory(prefix="f01_mutation_gate_") as directory:
        root = Path(directory)
        opt = root / "src" / "opt"
        analysis = root / "src" / "analysis"
        opt.mkdir(parents=True)
        analysis.mkdir(parents=True)
        src = opt / "Bad.cpp"
        good = """void good(IRMutator& mut, Instruction* i, Value* v) {
           mut.setOperand(i, 0, v);
        }"""
        src.write_text(good)
        assert run(root).returncode == 0, "normal IRMutator call rejected"
        for bad in forbidden:
            src.write_text("void bad() { " + bad + " }\n")
            result = run(root)
            assert result.returncode != 0, (bad, result.stdout)
        src.unlink()
        (analysis / "Bad.cpp").write_text("void bad() { x->setOperand(0, v); }\n")
        assert run(root).returncode != 0, "analysis mutation was not rejected"
        (analysis / "Bad.cpp").unlink()
        src.write_text("// i->setOperand(0, v);\n" + good)
        assert run(root).returncode == 0, "comments should not trigger"
    print(f"PASS: {len(forbidden) + 1} negatives and 2 positive controls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
