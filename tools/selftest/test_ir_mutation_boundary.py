#!/usr/bin/env python3
"""Negative and positive controls for the F01 optimizer mutation boundary."""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import sys

CHECK = Path(__file__).with_name("check_ir_mutation_boundary.py")


def run(root):
    return subprocess.run([sys.executable, str(CHECK), "--root", str(root)],
                          capture_output=True, text=True)


def main():
    raw_methods = (
        ("i", "setOperand", "0, v"),
        ("i", "clearOperands", ""),
        ("i", "addSucc", "b"),
        ("i", "setSucc", "0, b"),
        ("i", "clearSuccs", ""),
        ("b", "addInst", "i"),
        ("b", "insertInst", "0, i"),
        ("b", "removeInst", "i"),
        ("v", "replaceAllUsesWith", "w"),
    )
    forbidden = [
        f"{receiver}{access}{method}({args});"
        for receiver, method, args in raw_methods
        for access in ("->", ".")
    ]
    forbidden.extend((
        "(*i).setOperand(0, v);",
        "(*b).removeInst(i);",
        "m.rebuildCFG();",
        "m.rebuildUseDef();",
    ))
    good = """void good(IRMutator& mut, CFGEditor& cfg,
               Instruction* i, Value* v, Value* oldValue,
               Value* newValue, BasicBlock* b, BasicBlock* pred) {
        mut.rewriteOperand(i, 0, v);
        mut.replaceUses(oldValue, newValue);
        mut.eraseInstruction(i);
        mut.replaceAndErase(i, v);
        cfg.rewritePhiValue(i, pred, v);
        cfg.insertPhi(b, i);
    }
    """
    ignored = (
        "// i->setOperand(0, v); b.removeInst(i);\n"
        "/* (*i).setOperand(0, v);\n"
        "   m.rebuildCFG(); v->replaceAllUsesWith(w); */\n"
        'const char* example = "i.clearOperands(); b->addSucc(c); m.rebuildUseDef();";\n'
    )
    with TemporaryDirectory(prefix="f01_mutation_gate_") as directory:
        root = Path(directory)
        for folder in ("opt", "analysis"):
            path = root / "src" / folder
            path.mkdir(parents=True)
            src = path / "Boundary.cpp"
            src.write_text(good)
            result = run(root)
            assert result.returncode == 0, (folder, "tracked API rejected", result.stdout)
            for bad in forbidden:
                src.write_text("void bad() { " + bad + " }\n")
                result = run(root)
                assert result.returncode != 0, (folder, bad, result.stdout)
            src.write_text(ignored + good)
            result = run(root)
            assert result.returncode == 0, (folder, "comments/strings rejected", result.stdout)
            src.unlink()
    print(f"PASS: {len(forbidden) * 2} negatives and 4 positive controls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
