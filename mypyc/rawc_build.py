"""Raw C compilation pipeline for mypyc.

Compiles multiple Python modules together into raw C code without
any Python C API dependency. Entry points for the Python bridge
are marked with @mypyc_attr(rawc_export=True) in the source.

Usage:
    python -m mypyc.rawc_build path/to/module1.py path/to/module2.py ...
"""

from __future__ import annotations

import os
import sys

from mypy.build import BuildSource, build
from mypy.errors import CompileError
from mypy.fscache import FileSystemCache
from mypy.options import Options
from mypyc.codegen.emit_rawc import generate_rawc_c_for_module
from mypyc.errors import Errors
from mypyc.irbuild.main import build_ir
from mypyc.irbuild.rawc_mapper import RawcMapper
from mypyc.options import CompilerOptions
from mypyc.transform.flag_elimination import do_flag_elimination
from mypyc.transform.lower import lower_ir


def rawc_compile(
    source_paths: list[str], output_dir: str = "build", verbose: bool = False
) -> list[str]:
    """Compile Python modules to raw C code.

    Args:
        source_paths: Paths to Python source files to compile together.
        output_dir: Directory to write output files.
        verbose: Print progress info.

    Returns:
        List of rawc .c file paths. Bridge is generated separately by build.py
        using mypyc IR for correct struct offsets.
    """
    # Resolve module names from paths
    sources = []
    module_names = []
    parent_dir = None
    for path in source_paths:
        path = os.path.abspath(path)
        name = os.path.splitext(os.path.basename(path))[0]
        source_dir = os.path.dirname(path)
        if os.path.exists(os.path.join(source_dir, "__init__.py")):
            pkg_name = os.path.basename(source_dir)
            name = f"{pkg_name}.{name}"
        if parent_dir is None:
            parent_dir = os.path.dirname(source_dir)
        sources.append(BuildSource(path, name))
        module_names.append(name)

    # Set up mypy options
    options = Options()
    options.show_traceback = True
    options.strict_optional = True
    options.python_version = sys.version_info[:2]
    options.export_types = True
    options.preserve_asts = True
    options.incremental = False
    if parent_dir:
        options.mypy_path = [parent_dir]

    compiler_options = CompilerOptions(target_dir=output_dir, rawc=True, verbose=verbose)

    # Type check with mypy
    try:
        result = build(sources=sources, options=options, fscache=FileSystemCache())
    except CompileError as e:
        print(f"Type checking failed: {e}", file=sys.stderr)
        sys.exit(1)

    if result.errors:
        for error in result.errors:
            print(error, file=sys.stderr)
        sys.exit(1)

    # Create mapper — all modules share the same group
    group_map: dict[str, str | None] = {name: None for name in module_names}
    mapper = RawcMapper(group_map)

    # Build IR for all modules together
    errors = Errors(options)
    trees = []
    for st in result.graph.values():
        if st.id in module_names and st.tree:
            trees.append(st.tree)
    if not trees:
        print("No module trees found", file=sys.stderr)
        sys.exit(1)

    all_modules = build_ir(trees, result.graph, result.types, mapper, compiler_options, errors)
    if errors.num_errors > 0:
        print(f"IR generation failed with {errors.num_errors} errors", file=sys.stderr)
        sys.exit(1)

    # Resolve enum values from mypy AST (no runtime import needed)
    # Extract enum member values from the AST
    from mypy.nodes import AssignmentStmt, ClassDef, IntExpr
    from mypyc.codegen.emit_rawc import RawcFunctionEmitter

    enum_values: dict[str, int] = {}
    for tree in trees:
        for defn in tree.defs:
            if isinstance(defn, ClassDef) and defn.info.is_enum:
                cls_name = defn.name
                # IntEnum members are assignments with IntExpr values
                # Also handle auto() by tracking assignment order
                auto_counter = 1
                for stmt in defn.defs.body:
                    if isinstance(stmt, AssignmentStmt) and len(stmt.lvalues) == 1:
                        name_node = stmt.lvalues[0]
                        if hasattr(name_node, "name"):
                            member_name = name_node.name
                            key = f"{cls_name}.{member_name}"
                            if isinstance(stmt.rvalue, IntExpr):
                                enum_values[key] = stmt.rvalue.value
                                auto_counter = stmt.rvalue.value + 1
                            else:
                                # auto() — assign sequential value
                                enum_values[key] = auto_counter
                                auto_counter += 1

    if verbose and enum_values:
        print(f"  Resolved {len(enum_values)} enum values from AST")

    RawcFunctionEmitter._enum_values.update(enum_values)

    for mod_name, module_ir in all_modules.items():
        if mod_name not in module_names:
            continue
        # Apply transforms
        for fn in module_ir.functions:
            lower_ir(fn, compiler_options)
            do_flag_elimination(fn, compiler_options)

    # Emit raw C for each module
    os.makedirs(output_dir, exist_ok=True)
    rawc_paths = []
    is_first = True
    for mod_name in module_names:
        if mod_name not in all_modules:
            if verbose:
                print(f"  Skipping {mod_name} (no IR)")
            continue
        module_ir = all_modules[mod_name]
        rawc_code = generate_rawc_c_for_module(module_ir, is_primary=is_first)
        is_first = False
        rawc_path = os.path.join(output_dir, f"{mod_name}_rawc.c")
        with open(rawc_path, "w") as f:
            f.write(rawc_code)
        rawc_paths.append(rawc_path)
        if verbose:
            print(f"  Generated: {rawc_path}")

    return rawc_paths


def main() -> None:
    """CLI entry point: python -m mypyc.rawc_build module1.py module2.py ..."""
    import argparse

    parser = argparse.ArgumentParser(description="Compile Python to raw C via mypyc")
    parser.add_argument("sources", nargs="+", help="Python source files to compile")
    parser.add_argument("--output-dir", "-o", default="build", help="Output directory")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    rawc_paths = rawc_compile(args.sources, output_dir=args.output_dir, verbose=args.verbose)

    for p in rawc_paths:
        print(f"Raw C: {p}")


if __name__ == "__main__":
    main()
