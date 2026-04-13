"""Hoist loop-invariant string buffer reads.

When ``CPyStr_GetItemUnsafeAsInt`` is called repeatedly on the same string value,
each call reads ``PyUnicode_KIND`` and ``PyUnicode_DATA`` from the string's header.
Those values are loop-invariant because Python strings are immutable, so we can
read them once outside the loop and reuse them per iteration.

This pass is deliberately simple and targets the hot case: string values that
are function arguments (always loop-invariant within the function). For each
such argument used in ``CPyStr_GetItemUnsafeAsInt``, we emit a pair of
``CPyStr_LoadKind`` + ``CPyStr_LoadData`` calls at the top of the entry block
and rewrite every matching call to use the buffer-based variant.
"""

from __future__ import annotations

from mypyc.ir.func_ir import FuncIR
from mypyc.ir.ops import BasicBlock, CallC, Value
from mypyc.irbuild.ll_builder import LowLevelIRBuilder
from mypyc.options import CompilerOptions
from mypyc.primitives.str_ops import (
    str_get_item_from_buffer_op,
    str_load_data_op,
    str_load_kind_op,
)
from mypyc.transform.ir_transform import IRTransform, PatchVisitor, is_empty_block

GET_ITEM_AS_INT = "CPyStr_GetItemUnsafeAsInt"


def do_str_buffer_hoist(fn: FuncIR, options: CompilerOptions) -> None:
    # Collect CallC sites to CPyStr_GetItemUnsafeAsInt, grouped by string operand.
    calls_by_str: dict[Value, list[CallC]] = {}
    for block in fn.blocks:
        for op in block.ops:
            if isinstance(op, CallC) and op.function_name == GET_ITEM_AS_INT:
                s = op.args[0]
                calls_by_str.setdefault(s, []).append(op)

    if not calls_by_str:
        return

    # Conservatively hoist only when the string is a function argument — that
    # guarantees it is loop-invariant across the whole function. This covers
    # tight per-character loops like ``_hash_str(s)``. A follow-up can extend
    # to dominating defs for cases like ``self.sql`` caching.
    args_set = set(fn.arg_regs)
    hoist_targets: dict[Value, list[CallC]] = {
        s: calls for s, calls in calls_by_str.items() if s in args_set
    }
    if not hoist_targets:
        return

    builder = LowLevelIRBuilder(None, options)
    transform = StrBufferHoistTransform(builder, hoist_targets)
    transform.transform_blocks(fn.blocks)
    fn.blocks = builder.blocks


class StrBufferHoistTransform(IRTransform):
    def __init__(
        self, builder: LowLevelIRBuilder, hoist_targets: dict[Value, list[CallC]]
    ) -> None:
        super().__init__(builder)
        self.hoist_strs: list[Value] = list(hoist_targets.keys())
        # Populated during transform_blocks when the first block is activated.
        self.hoisted: dict[Value, tuple[Value, Value]] = {}
        # Lookup from the original CallC to its string operand.
        self.calls_to_rewrite: dict[CallC, Value] = {}
        for s, calls in hoist_targets.items():
            for call in calls:
                self.calls_to_rewrite[call] = s

    def visit_call_c(self, op: CallC) -> Value | None:
        s = self.calls_to_rewrite.get(op)
        if s is not None and s in self.hoisted:
            kind_val, data_val = self.hoisted[s]
            idx = op.args[1]
            return self.builder.primitive_op(
                str_get_item_from_buffer_op, [kind_val, data_val, idx], op.line
            )
        return self.add(op)

    def transform_blocks(self, blocks: list[BasicBlock]) -> None:
        # Mirror IRTransform.transform_blocks, but before visiting the ops of
        # the entry block, emit the hoisted LoadKind / LoadData calls.
        block_map: dict[BasicBlock, BasicBlock] = {}
        op_map = self.op_map
        empties: set[BasicBlock] = set()
        for i, block in enumerate(blocks):
            new_block = BasicBlock()
            block_map[block] = new_block
            self.builder.activate_block(new_block)
            new_block.error_handler = block.error_handler
            if i == 0:
                for s in self.hoist_strs:
                    kind_val = self.builder.primitive_op(str_load_kind_op, [s], -1)
                    data_val = self.builder.primitive_op(str_load_data_op, [s], -1)
                    self.hoisted[s] = (kind_val, data_val)
            for op in block.ops:
                new_op = op.accept(self)
                if new_op is not op:
                    op_map[op] = new_op
            if is_empty_block(new_block) and not is_empty_block(block):
                empties.add(new_block)

        self.builder.blocks = [b for b in self.builder.blocks if b not in empties]
        patcher = PatchVisitor(op_map, block_map)
        for block in self.builder.blocks:
            for op in block.ops:
                op.accept(patcher)
            if block.error_handler is not None:
                block.error_handler = block_map.get(block.error_handler, block.error_handler)
