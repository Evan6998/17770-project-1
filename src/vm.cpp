#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <list>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "vm.h"

namespace {


Value zero_value_for(wasm_type_t type) {
  switch (type) {
    case WASM_TYPE_I32:
      return static_cast<std::int32_t>(0);
    case WASM_TYPE_I64:
      return static_cast<std::int64_t>(0);
    case WASM_TYPE_F32:
      return 0.0f;
    case WASM_TYPE_F64:
      return 0.0;
    default:
      throw std::runtime_error("unsupported local type for zero initialisation");
  }
}

std::string value_to_string(const Value& value) {
  return std::visit([](auto&& arg) { return std::to_string(arg); }, value);
}

} // namespace

WasmVM::WasmVM(const WasmModule& module) : module_(module) {
  initialize_runtime_environment();
}

void WasmVM::run(std::vector<std::string> mainargs) {
  if (main_ == nullptr) {
    ERR("no main function found\n");
    return;
  }

  reset_runtime_state();

  // Prepare arguments for main function
  if (!validate_main_signature(mainargs.size())) {
    ERR("main function takes %lu arguments, but %lu were provided\n", 
        main_->sig->params.size(), mainargs.size());
    return;
  }

  push_main_arguments(mainargs);

  try
  {
    invoke(main_);
  }
  catch(const std::exception& e)
  {
    TRACE("Runtime error: %s\n", e.what());
    printf("!trap\n");
    return;
  }

  print_final_results();

}

void WasmVM::print_final_results() {
  /* * When printing f64 outputs, print them with precision of *6-digits after the decimal point*
   * Print all expected outputs (including *!trap*) to `stdout` only, and make sure `stderr` is empty for grading
  */
  if (!main_) {
    return;
  }
  std::vector<wasm_type_t> result_types(main_->sig->results.begin(), main_->sig->results.end());
  size_t result_count = result_types.size();
  if (result_count == 0) {
    return;
  }
  if (sp() != result_count) {
    throw std::runtime_error("Operand stack size does not match expected result count");
  }

  std::vector<Value> results;
  results.reserve(result_count);
  for (auto it = result_types.rbegin(); it != result_types.rend(); ++it) {
    TRACE("Result type: %s\n", wasm_type_string(*it));
    results.push_back(pop());
  }
  std::reverse(results.begin(), results.end());

  std::cout.precision(6);
  for (size_t i = 0; i < result_count; ++i) {
    const wasm_type_t type = result_types[i];
    const Value& value = results[i];
    if (type == WASM_TYPE_F64) {
      std::cout << std::fixed << std::get<double>(value) << std::endl;
    } else if (type == WASM_TYPE_F32) {
      std::cout << std::fixed << std::get<float>(value) << std::endl;
    } else {
      std::visit([](auto&& arg) { std::cout << arg << std::endl; }, value);
    }
  }
}

std::vector<Value> WasmVM::build_locals_for(const FuncDecl* f) {
  const size_t param_count = f->sig->params.size();
  if (sp() < param_count) {
    throw std::runtime_error("Not enough values on the operand stack for function parameters");
  }

  std::vector<Value> locals(param_count + f->num_pure_locals);

  for (size_t i = 0; i < param_count; ++i) {
    locals[param_count - 1 - i] = pop();
  }

  size_t next_local = param_count;
  for (const auto& group : f->pure_locals) {
    for (uint32_t i = 0; i < group.count; ++i) {
      locals[next_local++] = zero_value_for(group.type);
    }
  }
  return locals;
}

void WasmVM::skip_immediate(Opcode_t opcode, buffer_t &buf) {
  switch (opcode) {
    case WASM_OP_BLOCK:
    case WASM_OP_LOOP:
    case WASM_OP_IF: {
      RD_BYTE();
      break;
    }
    case WASM_OP_BR:
    case WASM_OP_BR_IF:
    case WASM_OP_CALL:
    case WASM_OP_LOCAL_GET:
    case WASM_OP_LOCAL_SET:
    case WASM_OP_LOCAL_TEE:
    case WASM_OP_GLOBAL_GET:
    case WASM_OP_GLOBAL_SET:
    case WASM_OP_MEMORY_SIZE:
    case WASM_OP_MEMORY_GROW: {
      RD_U32();
      break;
    }
    case WASM_OP_CALL_INDIRECT: {
      RD_U32();
      RD_U32();
      break;
    }
    case WASM_OP_BR_TABLE: {
      uint32_t target_count = RD_U32();
      for (uint32_t i = 0; i < target_count; ++i) {
        RD_U32();
      }
      RD_U32();
      break;
    }
    case WASM_OP_I32_LOAD:
    case WASM_OP_I64_LOAD:
    case WASM_OP_F32_LOAD:
    case WASM_OP_F64_LOAD:
    case WASM_OP_I32_LOAD8_S:
    case WASM_OP_I32_LOAD8_U:
    case WASM_OP_I32_LOAD16_S:
    case WASM_OP_I32_LOAD16_U:
    case WASM_OP_I32_STORE:
    case WASM_OP_I64_STORE:
    case WASM_OP_F32_STORE:
    case WASM_OP_F64_STORE:
    case WASM_OP_I32_STORE8:
    case WASM_OP_I32_STORE16: {
      RD_U32();
      RD_U32();
      break;
    }
    case WASM_OP_I32_CONST: {
      RD_I32();
      break;
    }
    case WASM_OP_I64_CONST: {
      RD_I64();
      break;
    }
    case WASM_OP_F32_CONST: {
      RD_U32_RAW();
      break;
    }
    case WASM_OP_F64_CONST: {
      RD_U64_RAW();
      break;
    }
    default:
      break;
  }
}

std::unordered_map<const byte*, CtrlMeta>  WasmVM::pre_indexing(FuncDecl* f) {
  std::unordered_map<const byte*, CtrlMeta> ctrl_map;
  auto ctrl_stack = std::vector<std::pair<const byte*, CtrlMeta>>{};
  // push implicit ctrl_meta for function body
  ctrl_stack.push_back({f->code_bytes.data(), CtrlMeta{Label::Kind::Block, nullptr, f->code_bytes.data() + f->code_bytes.size()}});
  const auto& bytes = f->code_bytes;
  auto buf = buffer_t{bytes.data(), bytes.data(), bytes.data() + bytes.size()};
  while (buf.ptr < buf.end) {
    const byte* opcode_ptr = buf.ptr;
    Opcode_t opcode = RD_OPCODE();
    TRACE("Pre-indexing opcode: %s at offset %ld\n", opcode_table[opcode].mnemonic, opcode_ptr - bytes.data());
    switch (opcode) {
      case WASM_OP_LOOP:
      case WASM_OP_IF:
      case WASM_OP_BLOCK: {
        // MVP only supports the empty blocktype (0x40). Consume it and push a label.
        uint8_t block_type = RD_BYTE();
        if (block_type != 0x40) {
          throw std::runtime_error("non-empty blocktype is not supported");
        }
        CtrlMeta meta{};
        // meta.begin = buf.ptr;
        meta.else_pc = nullptr;
        meta.end = nullptr; // to be filled when matching END is seen
        switch (opcode) {
          case WASM_OP_LOOP:
            meta.kind = Label::Kind::Loop;
            break;
          case WASM_OP_IF:
            meta.kind = Label::Kind::If;
            break;
          case WASM_OP_BLOCK:
            meta.kind = Label::Kind::Block;
            break;
          default:
            throw std::runtime_error("unreachable");
        }
        ctrl_stack.push_back({opcode_ptr, meta});
        break;
      }
      case WASM_OP_ELSE: {
        if (ctrl_stack.empty() || ctrl_stack.back().second.kind != Label::Kind::If) {
          throw std::runtime_error("else without matching if");
        }
        auto& [if_header, if_meta] = ctrl_stack.back();
        if_meta.else_pc = buf.ptr;
        break;
      }
      case WASM_OP_END: {
        if (ctrl_stack.empty()) {
          throw std::runtime_error("end without matching block/loop/if");
        }
        auto& [ctrl_header, meta] = ctrl_stack.back();
        ctrl_stack.pop_back();
        meta.end = opcode_ptr;
        ctrl_map[ctrl_header] = meta;
        break;
      }
      default:
        skip_immediate(opcode, buf);
        break;
    }
  }
  if (!ctrl_stack.empty()) {
    throw std::runtime_error("unmatched block/loop/if");
  }
  return ctrl_map;
}

void WasmVM::add_frame(FuncDecl* f) {
  auto ctrl_map = pre_indexing(f);
  byte* start = f->code_bytes.data();
  byte* end = start + f->code_bytes.size();
  
  Frame frame{};
  frame.func = f;
  frame.locals = build_locals_for(f);
  frame.pc.start = start;
  frame.pc.ptr = start;
  frame.pc.end = end;
  frame.stack_height_on_entry = sp();
  frame.ctrl_map = std::move(ctrl_map);

  Label function_body{};
  function_body.kind = Label::Kind::Implicit;
  // function_body.pc_begin = start;
  // function_body.pc_end = end;
  function_body.pc_else = nullptr;
  function_body.stack_height = frame.stack_height_on_entry;
  frame.labels.push_back(function_body);

  TRACE("Invoking function with %zu locals\n", frame.locals.size());
  for (size_t i = 0; i < frame.locals.size(); ++i) {
    const std::string repr = value_to_string(frame.locals[i]);
    TRACE("  local[%zu]: %s\n", i, repr.c_str());
  }

  call_stack_.push_back(std::move(frame));
  TRACE("Pushed function frame onto call stack\n");

}

bool WasmVM::invoke(FuncDecl* f) {
  add_frame(f);
  
  while (!call_stack_.empty()) {
    run_op();
  }
  return true;
}

void WasmVM::run_op() {
  if (call_stack_.empty()) {
    throw std::runtime_error("Call stack underflow");
  }
  Frame& frame = call_stack_.back();
  buffer_t &buf = frame.pc;
  auto &ctrl_map = frame.ctrl_map;
  // if reach end of buffer, pop the call stack and return
  if (buf.ptr >= buf.end) {
    throw std::runtime_error("Reached end of buffer");
  }
  auto header = buf.ptr;
  // trace value of buf.ptr
  // TRACE("Running to: %p\n", (void*)buf.ptr);
  Opcode_t opcode = RD_OPCODE();
  switch (opcode) {
    case WASM_OP_I32_CONST: {
      auto v = RD_I32();
      TRACE("I32_CONST: %d\n", v);
      push(v);
      break;
    }
    case WASM_OP_I64_CONST: {
      auto v = RD_I64();
      TRACE("I64_CONST: %lld\n", v);
      push(v);
      break;
    }
    case WASM_OP_F32_CONST: {
      uint32_t raw = RD_U32_RAW();
      float v = raw_to_f32(raw);
      TRACE("F32_CONST: %f\n", v);
      push(v);
      break;
    }
    case WASM_OP_F64_CONST: {
      uint64_t raw = RD_U64_RAW();
      double v = raw_to_f64(raw);
      TRACE("F64_CONST: %f\n", v);
      push(v);
      break;
    }
    case WASM_OP_LOCAL_GET: {
      uint32_t local_idx = RD_U32();
      Frame& current_frame = call_stack_.back();
      if (local_idx >= current_frame.locals.size()) {
        throw std::runtime_error("local.get index out of bounds");
      }
      Value local_value = current_frame.locals[local_idx];
      const std::string repr = value_to_string(local_value);
      TRACE("LOCAL_GET: index %u value %s\n", local_idx, repr.c_str());
      push(local_value);
      break;
    }
    case WASM_OP_LOCAL_SET: {
      auto local_idx = RD_U32();
      auto& frame = call_stack_.back();
      auto value = pop();
      if (local_idx >= frame.locals.size()) {
        throw std::runtime_error("local.set index out of bounds");
      }
      frame.locals[local_idx] = value;
      TRACE("LOCAL_SET: index %u value %s\n", local_idx, value_to_string(value).c_str());
      break;
    }
    case WASM_OP_LOCAL_TEE: {
      auto local_idx = RD_U32();
      auto& frame = call_stack_.back();
      auto value = pop();
      if (local_idx >= frame.locals.size()) {
        throw std::runtime_error("local.tee index out of bounds");
      }
      frame.locals[local_idx] = value;
      push(value);
      TRACE("LOCAL_TEE: index %u value %s\n", local_idx, value_to_string(value).c_str());
      break;
    }
    case WASM_OP_BLOCK: {
      // MVP only supports the empty blocktype (0x40). Consume it and push a label.
      uint8_t block_type = RD_BYTE();
      if (block_type != 0x40) {
        throw std::runtime_error("non-empty blocktype is not supported");
      }

      Frame& current_frame = call_stack_.back();
      Label block{};
      block.kind = Label::Kind::Block;
      // block.pc_begin = buf.ptr;   // execution continues with the following instruction
      block.pc_target = ctrl_map.at(header).end;
      block.pc_else = nullptr;
      block.stack_height = sp();
      current_frame.labels.push_back(block);
      TRACE("BLOCK: depth %zu\n", current_frame.labels.size());
      break;
    }
    case WASM_OP_LOOP: {
      uint8_t block_type = RD_BYTE();
      if (block_type != 0x40) {
        throw std::runtime_error("non-empty blocktype is not supported");
      }

      Frame& current_frame = call_stack_.back();
      Label loop{};
      loop.kind = Label::Kind::Loop;
      loop.pc_target = buf.ptr;   // execution continues with the following instruction
      loop.pc_else = nullptr;
      loop.stack_height = sp();
      current_frame.labels.push_back(loop);
      TRACE("LOOP: depth %zu\n", current_frame.labels.size());
      break;
    }
    case WASM_OP_IF: {
      auto block_type = RD_BYTE();
      if (block_type != 0x40) {
        throw std::runtime_error("non-empty blocktype is not supported");
      }
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for if condition");
      }

      Frame& current_frame = call_stack_.back();
      Label if_label{};
      if_label.kind = Label::Kind::If;
      if_label.pc_target = ctrl_map.at(header).end;
      if_label.pc_else = nullptr;
      if_label.stack_height = sp();
      current_frame.labels.push_back(if_label);
      
      Value condition = pop();
      if (!std::holds_alternative<std::int32_t>(condition)) {
        throw std::runtime_error("Condition for if is not i32");
      }
      bool cond = std::get<std::int32_t>(condition) != 0;
      if (!cond) {
        if (ctrl_map.at(header).else_pc) {
          buf.ptr = ctrl_map.at(header).else_pc; // enter the 'else' branch
          TRACE("condition false, entering ELSE branch\n");
        } else {
          // No else branch, skip to the end of the if
          buf.ptr = if_label.pc_target;
          // current_frame.labels.pop_back(); // pop the if label
          TRACE("condition false, skipping to END\n");
        }
      }
      TRACE("IF: condition %d, depth %zu\n", std::get<std::int32_t>(condition), current_frame.labels.size());
      break;
    }
    case WASM_OP_ELSE: {
      if (call_stack_.empty()) {
        throw std::runtime_error("Empty call stack on else");
      }
      auto& current_frame = call_stack_.back();
      if (current_frame.labels.empty() || current_frame.labels.back().kind != Label::Kind::If) {
        throw std::runtime_error("else without matching if");
      }
      auto& if_label = current_frame.labels.back();
      buf.ptr = if_label.pc_target; // skip to the end
      current_frame.labels.pop_back(); // pop the if label
      TRACE("ELSE\n");
      break;
    }
    case WASM_OP_I32_LT_S: {
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for i32.lt_s");
      }
      Value val2 = pop();
      Value val1 = pop();
      auto result = std::get<std::int32_t>(val1) < std::get<std::int32_t>(val2) ? 1 : 0;
      TRACE("I32_LT_S: %d < %d = %d\n", std::get<std::int32_t>(val1), std::get<std::int32_t>(val2), result);
      push(static_cast<Value>(result));
      break;
    }
    case WASM_OP_I32_EQZ: {
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for i32.eqz");
      }
      Value val = pop();
      auto result = std::get<std::int32_t>(val) == 0 ? 1 : 0;
      push(static_cast<Value>(result));
      TRACE("I32_EQZ: %d == 0 = %d\n", std::get<std::int32_t>(val), result);
      break;
    }
    case WASM_OP_I32_ADD: {
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for i32.add");
      }
      Value val2 = pop();
      Value val1 = pop();
      auto result = std::get<std::int32_t>(val1) + std::get<std::int32_t>(val2);
      TRACE("I32_ADD: %d + %d = %d\n", std::get<std::int32_t>(val1), std::get<std::int32_t>(val2), result);
      push(static_cast<Value>(result));
      break;
    }
    case WASM_OP_I32_SUB: {
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for i32.sub");
      }
      Value val2 = pop();
      Value val1 = pop();
      auto result = std::get<std::int32_t>(val1) - std::get<std::int32_t>(val2);
      TRACE("I32_SUB: %d - %d = %d\n", std::get<std::int32_t>(val1), std::get<std::int32_t>(val2), result);
      push(static_cast<Value>(result));
      break;
    }
    case WASM_OP_I32_LOAD: {
      uint32_t align = RD_U32();
      uint32_t offset = RD_U32();
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for i32.load");
      }
      Value addr_val = pop();
      if (!std::holds_alternative<std::int32_t>(addr_val)) {
        throw std::runtime_error("Address for i32.load is not i32");
      }
      if (std::get<std::int32_t>(addr_val) < 0) {
        throw std::runtime_error("Address for i32.load is negative");
      }
      uint32_t addr = static_cast<uint32_t>(std::get<std::int32_t>(addr_val));
      uint32_t effective_addr = addr + offset;
      if (effective_addr + 4 > linear_memory_.size()) {
        throw std::runtime_error("i32.load address out of bounds");
      }
      uint32_t loaded = 0;
      std::memcpy(&loaded, &linear_memory_[effective_addr], sizeof(int32_t));
      push(static_cast<Value>(static_cast<std::int32_t>(loaded)));
      TRACE("I32_LOAD: align %u offset %u addr %u (eff %u) => %d\n", align, offset, addr, effective_addr, std::get<std::int32_t>(top()));
      break;
    }
    case WASM_OP_I32_STORE: {
      uint32_t align = RD_U32();
      uint32_t offset = RD_U32();
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for i32.store");
      }
      Value val = pop();
      Value addr_val = pop();
      if (!std::holds_alternative<std::int32_t>(addr_val)) {
        throw std::runtime_error("Address for i32.store is not i32");
      }
      if (std::get<std::int32_t>(addr_val) < 0) {
        throw std::runtime_error("Address for i32.store is negative");
      }
      uint32_t addr = static_cast<uint32_t>(std::get<std::int32_t>(addr_val));
      uint32_t effective_addr = addr + offset;
      if (effective_addr + 4 > linear_memory_.size()) {
        throw std::runtime_error("i32.store address out of bounds");
      }
      uint32_t to_store = static_cast<uint32_t>(std::get<std::int32_t>(val));
      std::memcpy(&linear_memory_[effective_addr], &to_store, sizeof(int32_t));
      TRACE("I32_STORE: align %u offset %u addr %u (eff %u) <= %d\n", align, offset, addr, effective_addr, std::get<std::int32_t>(val));
      break;
    }
    case WASM_OP_I32_EQ: {
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for i32.eq");
      }
      Value val2 = pop();
      Value val1 = pop();
      auto result = std::get<std::int32_t>(val1) == std::get<std::int32_t>(val2) ? 1 : 0;
      TRACE("I32_EQ: %d == %d = %d\n", std::get<std::int32_t>(val1), std::get<std::int32_t>(val2), result);
      push(static_cast<Value>(result));
      break;
    }
    case WASM_OP_F64_ADD: {
      if (sp() < 2) {
        throw std::runtime_error("Not enough values on the operand stack for f64.add");
      }
      Value val2 = pop();
      Value val1 = pop();
      auto result = std::get<double>(val1) + std::get<double>(val2);
      TRACE("F64_ADD: %f + %f = %f\n", std::get<double>(val1), std::get<double>(val2), result);
      push(result);
      break;
    }
    case WASM_OP_NOP: {
      break;
    }
    case WASM_OP_UNREACHABLE: {
      throw std::runtime_error("unreachable executed");
      break;
    }
    case WASM_OP_END: {
      // Close the nearest structured control construct (block/loop/if/function).
      Frame& fr = call_stack_.back();
      if (fr.labels.empty()) {
        throw std::runtime_error("END encountered with no active label");
      }
      Label& closed = fr.labels.back();
      fr.labels.pop_back();

      // If we just closed the implicit function-body label, we must PRESERVE the function results
      // BEFORE restoring the operand stack height; otherwise they'd be lost.
      const bool is_function_end = fr.labels.empty();
      if (is_function_end) {
        const size_t retc = fr.func->sig->results.size();
        if (sp() < retc) {
          throw std::runtime_error("Not enough values on the operand stack for function return");
        }

        // Grab return values from the top of the stack first.
        std::vector<Value> rets;
        rets.reserve(retc);
        for (size_t i = 0; i < retc; ++i) {
          rets.push_back(pop());
        }
        std::reverse(rets.begin(), rets.end());

        // Restore the caller's operand stack height, pop the frame, then push back returns.
        pop_to(fr.stack_height_on_entry);
        call_stack_.pop_back();
        TRACE("Popping function frame, returning %lu values\n", rets.size());
        for (auto& v : rets) {
          push(v);
        }
        TRACE("Function return with %lu values\n", rets.size());
      } else {
        // Non-function structured end. In this project, blocks have 0 result arity,
        // so we simply restore the operand stack height to what it was at block entry.
        pop_to(closed.stack_height);
      }
      break;
    }
    case WASM_OP_RETURN: {
      Frame& fr = call_stack_.back();
      const size_t retc = fr.func->sig->results.size();
      if (sp() < retc) {
        throw std::runtime_error("Not enough values on the operand stack for function return");
      }

      // Grab return values from the top of the stack first.
      std::vector<Value> rets;
      rets.reserve(retc);
      for (size_t i = 0; i < retc; ++i) {
        rets.push_back(pop());
      }
      std::reverse(rets.begin(), rets.end());

      // Restore the caller's operand stack height, pop the frame, then push back returns.
      pop_to(fr.stack_height_on_entry);
      call_stack_.pop_back();
      TRACE("RETURN: popping function frame, returning %lu values\n", rets.size());
      for (auto& v : rets) {
        push(v);
      }
      TRACE("Function return with %lu values\n", rets.size());
      break;
    }
    case WASM_OP_CALL: {
      auto func_idx = RD_U32();
      if (func_idx >= function_instances_.size()) {
        throw std::runtime_error("call function index out of bounds");
      }
      FuncDecl* f = function_instances_[func_idx];
      add_frame(f);
      TRACE("CALL: function index %u\n", func_idx);
      break;
    }
    case WASM_OP_CALL_INDIRECT: {
      uint32_t type_index = RD_U32();
      uint32_t table_index = RD_U32();

      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for call_indirect");
      }

      Value table_elem = pop();
      if (!std::holds_alternative<std::int32_t>(table_elem)) {
        throw std::runtime_error("call_indirect index is not i32");
      }

      int32_t signed_idx = std::get<std::int32_t>(table_elem);
      if (signed_idx < 0) {
        throw std::runtime_error("call_indirect index out of bounds");
      }
      uint32_t elem_index = static_cast<uint32_t>(signed_idx);

      const uint32_t imported_tables = module_.get_num_imported_tables();
      if (table_index < imported_tables) {
        throw std::runtime_error("call_indirect into imported table not supported");
      }

      uint32_t local_table_index = table_index - imported_tables;
      if (local_table_index >= table_instances_.size()) {
        throw std::runtime_error("call_indirect table index out of bounds");
      }

      auto& table = table_instances_[local_table_index];
      if (elem_index >= table.size()) {
        throw std::runtime_error("call_indirect table element out of bounds");
      }

      FuncDecl* target = table[elem_index];
      if (target == nullptr) {
        throw std::runtime_error("call_indirect null table entry");
      }

      SigDecl* expected_sig = module_.getSig(type_index);
      if (expected_sig == nullptr) {
        throw std::runtime_error("call_indirect bad type index");
      }

      if (*(target->sig) != *expected_sig) {
        throw std::runtime_error("call_indirect signature mismatch");
      }

      TRACE("CALL_INDIRECT: table %u index %u\n", table_index, elem_index);
      add_frame(target);
      break;
    }
    case WASM_OP_DROP: {
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for drop");
      }
      Value dropped = pop();
      const std::string repr = value_to_string(dropped);
      TRACE("DROP: %s\n", repr.c_str());
      break;
    }
    case WASM_OP_SELECT: {
      if (sp() < 3) {
        throw std::runtime_error("Not enough values on the operand stack for select");
      }
      Value condition = pop();
      if (!std::holds_alternative<std::int32_t>(condition)) {
        throw std::runtime_error("Condition for select is not i32");
      }
      Value val2 = pop();
      Value val1 = pop();
      bool cond = std::get<std::int32_t>(condition) != 0;
      Value selected = cond ? val1 : val2;
      const std::string repr = value_to_string(selected);
      TRACE("SELECT: condition %d, selected %s\n", std::get<std::int32_t>(condition), repr.c_str());
      push(selected);
      break;
    }
    case WASM_OP_BR: {
      auto label_idx = RD_U32();
      Frame& fr = call_stack_.back();
      // trace these indexes
      if (label_idx >= fr.labels.size()) {
        throw std::runtime_error("br label index out of bounds");
      }
      Label& target_label = fr.labels[fr.labels.size() - label_idx - 1];
      // trace label kind
      fr.labels.resize(fr.labels.size() - label_idx); // pop labels up to and including target
      operand_stack_.resize(target_label.stack_height); // restore operand stack height
      buf.ptr = target_label.pc_target;
      TRACE("BR to label index %u of kind %d (total depth %zu)\n", label_idx, static_cast<int>(target_label.kind), fr.labels.size());
      break;
    }
    case WASM_OP_BR_IF: {
      auto label_idx = RD_U32();
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for br_if");
      }
      auto cond = pop();
      TRACE("BR_IF condition %d\n", std::get<std::int32_t>(cond));
      if (!std::holds_alternative<std::int32_t>(cond)) {
        throw std::runtime_error("Condition for br_if is not i32");
      }
      if (std::get<std::int32_t>(cond) != 0) {
        Frame& fr = call_stack_.back();
        if (label_idx >= fr.labels.size()) {
          throw std::runtime_error("br_if label index out of bounds");
        }
        Label& target_label = fr.labels[fr.labels.size() - label_idx - 1];
        // trace value of buf.ptr
        TRACE("BR_IF to buf.ptr %p\n", (void*)buf.ptr);
        buf.ptr = target_label.pc_target;
        fr.labels.pop_back(); // pop labels up to and including target
        TRACE("BR_IF to label index %u of kind %d (total depth %zu)\n", label_idx, static_cast<int>(target_label.kind), fr.labels.size());
      } else {
        TRACE("BR_IF not taken\n");
      }
      break;
    }
    case WASM_OP_GLOBAL_GET: {
      auto global_idx = RD_U32();
      if (global_idx >= global_values_.size()) {
        throw std::runtime_error("global.get index out of bounds");
      }
      Value global_value = global_values_[global_idx];
      const std::string repr = value_to_string(global_value);
      TRACE("GLOBAL_GET: index %u value %s\n", global_idx, repr.c_str());
      push(global_value);
      break;
    }
    case WASM_OP_GLOBAL_SET: {
      auto global_idx = RD_U32();
      auto value = pop();
      if (global_idx >= global_values_.size()) {
        throw std::runtime_error("global.set index out of bounds");
      }
      global_values_[global_idx] = value;
      TRACE("GLOBAL_SET: index %u value %s\n", global_idx, value_to_string(value).c_str());
      break;
    }

    default:
      ERR("Unknown init expr opcode: %x(%s)\n", opcode, opcode_table[opcode].mnemonic);
      throw std::runtime_error("Opcode error");
  }

}


void WasmVM::initialize_runtime_environment() {
  cache_linear_memory_layout();
  cache_table_layout();
  resolve_main_entrypoint();
}

void WasmVM::cache_linear_memory_layout() {
  initial_linear_memory_pages_ = 0;
  if (module_.get_num_mems() > module_.get_num_imported_mems()) {
    auto idx = module_.get_num_imported_mems();
    initial_linear_memory_pages_ = module_.getMemory(idx)->limits.initial;
  }
}

void WasmVM::cache_table_layout() {
  local_table_initial_sizes_.clear();
  const uint32_t total_tables = module_.get_num_tables();
  const uint32_t imported_tables = module_.get_num_imported_tables();
  if (total_tables > imported_tables) {
    local_table_initial_sizes_.reserve(total_tables - imported_tables);
    for (auto idx = imported_tables; idx < total_tables; idx++) {
      auto table_size = module_.getTable(idx)->limits.initial;
      local_table_initial_sizes_.push_back(table_size);
    }
  }
}

void WasmVM::resolve_main_entrypoint() {
  auto exports = module_.Exports();
  auto it = std::find_if(exports.begin(), exports.end(),
      [](auto const& exp) { return exp.name == "main" && exp.kind == KIND_FUNC; });
  main_ = (it != exports.end()) ? it->desc.func : nullptr;
}

void WasmVM::prepare_globals_storage() {
  global_values_.clear();
  global_values_.reserve(module_.Globals().size());
  for (const auto& glob : module_.Globals()) {
    global_values_.push_back(glob.init_value);
  }
  // trace all values in global_values_
  TRACE("Number of globals: %zu\n", global_values_.size());
  for (size_t i = 0; i < global_values_.size(); ++i) {
    const std::string repr = value_to_string(global_values_[i]);
    TRACE("  global[%zu]: %s\n", i, repr.c_str());
  }
}

void WasmVM::prepare_data_segments() {
  for (const auto& seg : module_.Datas()) {
    uint32_t offset = seg.mem_offset;
    if (offset + seg.bytes.size() > linear_memory_.size()) {
      throw std::runtime_error("Data segment does not fit in linear memory");
    }
    std::memcpy(&linear_memory_[offset], seg.bytes.data(), seg.bytes.size());
  }
}

void WasmVM::prepare_function_instances() {
  function_instances_.clear();
  function_instances_.reserve(module_.Funcs().size());
  for (auto& func : module_.Funcs()) {
    function_instances_.push_back(&func);
  }
}

void WasmVM::prepare_element_segments() {
  if (module_.get_num_tables() == 0) {
    return;
  }

  const uint32_t imported_tables = module_.get_num_imported_tables();
  for (const auto& elem : module_.Elems()) {
    // In the MVP subset we only handle active segments targeting table 0.
    const uint32_t table_index = 0;
    if (table_index < imported_tables) {
      throw std::runtime_error("Imported tables are not supported for element segments");
    }

    const uint32_t local_table_index = table_index - imported_tables;
    if (local_table_index >= table_instances_.size()) {
      throw std::runtime_error("Element segment references missing table");
    }

    auto& table = table_instances_[local_table_index];
    uint64_t offset = elem.table_offset;
    if (offset > table.size()) {
      throw std::runtime_error("Element segment offset outside table bounds");
    }

    size_t cursor = static_cast<size_t>(offset);
    for (auto func_ptr : elem.func_indices) {
      if (cursor >= table.size()) {
        throw std::runtime_error("Element segment exceeds table bounds");
      }
      table[cursor++] = func_ptr;
    }
  }
}

void WasmVM::reset_runtime_state() {
  linear_memory_.assign(initial_linear_memory_pages_ * WASM_PAGE_SIZE, 0);

  table_instances_.clear();
  table_instances_.reserve(local_table_initial_sizes_.size());
  for (auto table_size : local_table_initial_sizes_) {
    table_instances_.emplace_back(table_size, nullptr);
  }

  operand_stack_.clear();
  call_stack_.clear();
  prepare_globals_storage();
  prepare_data_segments();
  prepare_function_instances();
  prepare_element_segments();
}

bool WasmVM::validate_main_signature(size_t argc) const {
  return main_ && main_->sig->params.size() == argc;
}

void WasmVM::push_main_arguments(const std::vector<std::string>& mainargs) {
  auto it = main_->sig->params.begin();
  for (size_t i = 0; i < mainargs.size(); i++) {
    auto type = *(it++);
    operand_stack_.push_back(make_from(mainargs[i], type));
  }
}
