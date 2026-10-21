#!/usr/bin/env python3
"""Negative and positive controls for the F01 optimizer mutation boundary."""
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
import subprocess
import sys

CHECK = Path(__file__).with_name("check_ir_mutation_boundary.py")


def run(root):
    return subprocess.run([sys.executable, str(CHECK), "--root", str(root)],
                          capture_output=True, text=True)


def run_default(script, cwd):
    return subprocess.run([sys.executable, str(script)], cwd=str(cwd),
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
            for other in ("opt", "analysis"):
                (root / "src" / other).mkdir(parents=True, exist_ok=True)
            path = root / "src" / folder
            src = path / "Boundary.cpp"
            companion = root / "src" / ("analysis" if folder == "opt" else "opt") / "Companion.cpp"
            companion.write_text(good)
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
            companion.unlink()
        for folder in ("opt", "analysis"):
            (root / "src" / folder).rmdir()
        (root / "src").rmdir()
        compiler = root / "compiler"
        (compiler / "src" / "opt").mkdir(parents=True)
        (compiler / "src" / "analysis").mkdir(parents=True)
        (compiler / "src" / "opt" / "Good.cpp").write_text(good)
        (compiler / "src" / "analysis" / "Good.cpp").write_text(good)
        copied_checker = compiler / "tools" / "selftest" / CHECK.name
        copied_checker.parent.mkdir(parents=True)
        shutil.copy2(CHECK, copied_checker)
        unrelated = root / "unrelated-cwd"
        unrelated.mkdir()
        bad = compiler / "src" / "opt" / "Bad.cpp"
        bad.write_text("void bad() { i->addInst(x); }\n")
        result = run_default(copied_checker, unrelated)
        assert result.returncode != 0, "default checker must use its script root"
        assert "src/opt/Bad.cpp" in result.stdout, result.stdout
        bad.unlink()
        assert run(compiler).returncode == 0, "standalone compiler root accepted"
        assert run(root).returncode == 0, "workspace/compiler fallback accepted"

        missing = root / "missing"
        result = run(missing)
        assert result.returncode != 0 and "source root not found" in result.stdout
        empty = root / "empty"
        (empty / "src" / "opt").mkdir(parents=True)
        (empty / "src" / "analysis").mkdir(parents=True)
        result = run(empty)
        assert result.returncode != 0
        assert "src/opt: source directory is empty" in result.stdout
        assert "src/analysis: source directory is empty" in result.stdout
        partial = root / "partial"
        (partial / "src" / "opt").mkdir(parents=True)
        (partial / "src" / "opt" / "Good.cpp").write_text(good)
        result = run(partial)
        assert result.returncode != 0 and "src/analysis: source directory missing" in result.stdout
    print(f"PASS: {len(forbidden) * 2} negatives and 7 positive/layout controls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
