"""Fold ``char = Unbox(CPyStr_GetItem(s, i))`` into a direct int32 read.

When a char Register is defined by unboxing the result of ``CPyStr_GetItem``
(or ``CPyStr_GetItemUnsafe``), the default path allocates a 1-character
PyObject, runs a type/length check to unbox it, and frees the PyObject — all
per iteration. This pass replaces that sequence with ``CPyStr_GetCharAt`` (or
``CPyStr_GetCharAtUnsafe``) which reads the codepoint directly as an int32.

The pass preserves error semantics: ``CPyStr_GetCharAt`` returns ``-113`` on
out-of-range / non-str inputs, matching the error sentinel mypyc expects for
int32-typed results.
"""

from __future__ import annotations

from mypyc.ir.deps import STR_EXTRA_OPS
from mypyc.ir.func_ir import FuncIR
from mypyc.ir.ops import Branch, CallC, DecRef, Goto, IncRef, Op, Unbox, Value
from mypyc.ir.rtypes import is_char_rprimitive
from mypyc.options import CompilerOptions

STR_INDEXERS = {
    "CPyStr_GetItem": "CPyStr_GetCharAt",
    # The unsafe variant fires for patterns like ``for c in s: ...`` where
    # c is typed as char — mypyc emits CPyStr_GetItemUnsafe for str
    # iteration, then unboxes the 1-char result to char.
    "CPyStr_GetItemUnsafe": "CPyStr_GetCharAtUnsafe",
}


def do_char_str_index_fold(fn: FuncIR, options: CompilerOptions) -> None:
    # Build a use-map once: for each Value, all ops that consume it as a
    # source. This lets the compatibility check for each Unbox be O(uses)
    # rather than O(total ops). The map is a snapshot — it is NOT kept in
    # sync with the rewrite below; only read during candidate collection.
    uses: dict[Value, list[Op]] = {}
    unbox_targets: list[Unbox] = []
    for block in fn.blocks:
        for op in block.ops:
            if isinstance(op, Unbox) and is_char_rprimitive(op.type):
                unbox_targets.append(op)
            for src in op.sources():
                uses.setdefault(src, []).append(op)

    # Candidates: Unbox to char whose source is a str-indexing CallC and
    # whose only non-Unbox consumers are at most a single IS_ERROR Branch
    # plus any number of IncRef/DecRef ops.
    to_rewrite: list[tuple[CallC, Unbox]] = []
    call_c_results: set[Value] = set()
    for unbox in unbox_targets:
        src = unbox.src
        if not isinstance(src, CallC) or src.function_name not in STR_INDEXERS:
            continue
        compatible = True
        for consumer in uses.get(src, ()):
            if consumer is unbox:
                continue
            if isinstance(consumer, Branch) and consumer.op == Branch.IS_ERROR:
                continue
            if isinstance(consumer, (IncRef, DecRef)):
                continue
            compatible = False
            break
        if not compatible:
            continue
        to_rewrite.append((src, unbox))
        call_c_results.add(src)

    if not to_rewrite:
        return

    # Mutate each str-indexing CallC in place to return char. Preserving the
    # CallC identity means any existing IS_ERROR Branch keeps referencing the
    # same op — its check transparently switches from NULL (for PyObject*)
    # to -113 (for int32) because mypyc emits IS_ERROR based on the type.
    for call_c, unbox in to_rewrite:
        call_c.function_name = STR_INDEXERS[call_c.function_name]
        call_c.type = unbox.type
        # The new C helper lives in str_extra_ops.
        deps = list(call_c.dependencies) if call_c.dependencies else []
        if STR_EXTRA_OPS not in deps:
            deps.append(STR_EXTRA_OPS)
            call_c.dependencies = deps

    # The Unbox's own IS_ERROR Branch is now redundant — the CallC already
    # checks the sentinel. Replace those Branches with a Goto to the
    # continue path so we don't double-check.
    unboxes_to_remove = {unbox for _, unbox in to_rewrite}
    branches_to_drop: set[Op] = set()
    for unbox in unboxes_to_remove:
        for consumer in uses.get(unbox, ()):
            if isinstance(consumer, Branch) and consumer.op == Branch.IS_ERROR:
                branches_to_drop.add(consumer)

    # Redirect any remaining references to an Unbox onto the underlying
    # (now-char-typed) CallC, then drop the Unbox ops and the CallC's
    # IncRef/DecRef (char is not refcounted).
    unbox_to_callc = {unbox: call_c for call_c, unbox in to_rewrite}
    for block in fn.blocks:
        new_ops: list[Op] = []
        for op in block.ops:
            if op in unboxes_to_remove:
                continue
            if isinstance(op, (IncRef, DecRef)) and op.src in call_c_results:
                continue
            if op in branches_to_drop:
                assert isinstance(op, Branch)
                new_ops.append(Goto(op.false, op.line))
                continue
            srcs = op.sources()
            if any(isinstance(s, Unbox) and s in unbox_to_callc for s in srcs):
                op.set_sources(
                    [
                        unbox_to_callc[s] if isinstance(s, Unbox) and s in unbox_to_callc else s
                        for s in srcs
                    ]
                )
            new_ops.append(op)
        block.ops = new_ops
