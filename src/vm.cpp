#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vm.h"

namespace {

float raw_to_f32(uint32_t raw) {
  float value;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

double raw_to_f64(uint64_t raw) {
  double value;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

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

  if (!invoke(main_)) {
    ERR("failed to invoke main function\n");
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

bool WasmVM::invoke(FuncDecl* f) {
  byte* start = f->code_bytes.data();
  byte* end = start + f->code_bytes.size();
  
  Frame frame{};
  frame.func = f;
  frame.locals = build_locals_for(f);
  frame.pc.start = start;
  frame.pc.ptr = start;
  frame.pc.end = end;
  frame.stack_height_on_entry = sp();

  Label function_body{};
  function_body.kind = Label::Kind::Block;
  function_body.pc_begin = start;
  function_body.pc_end = end;
  function_body.pc_else = nullptr;
  function_body.stack_height = frame.stack_height_on_entry;
  frame.labels.push_back(function_body);

  TRACE("Invoking function with %zu locals\n", frame.locals.size());
  for (size_t i = 0; i < frame.locals.size(); ++i) {
    const std::string repr = value_to_string(frame.locals[i]);
    TRACE("  local[%zu]: %s\n", i, repr.c_str());
  }

  call_stack_.push_back(std::move(frame));

  // TODO: implement the function body execution
  while (!call_stack_.empty()) {
    auto& current_frame = call_stack_.back();
    run_op(current_frame.pc);
  }
  return true;
}

void WasmVM::run_op(buffer_t &buf) {
  // if reach end of buffer, pop the call stack and return
  if (buf.ptr >= buf.end) {
    throw std::runtime_error("Reached end of buffer");
  }

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
    case WASM_OP_BLOCK: {
      // MVP only supports the empty blocktype (0x40). Consume it and push a label.
      uint8_t block_type = RD_BYTE();
      if (block_type != 0x40) {
        throw std::runtime_error("non-empty blocktype is not supported");
      }

      Frame& current_frame = call_stack_.back();
      Label block{};
      block.kind = Label::Kind::Block;
      block.pc_begin = buf.ptr;   // execution continues with the following instruction
      block.pc_end = nullptr;     // resolved when matching END is seen or by future br setup
      block.pc_else = nullptr;
      block.stack_height = sp();
      current_frame.labels.push_back(block);
      TRACE("BLOCK: depth %zu\n", current_frame.labels.size());
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
      Label closed = fr.labels.back();
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

    default:
      ERR("Unknown init expr opcode: %x(%s)\n", opcode, opcode_table[opcode].mnemonic);
      throw std::runtime_error("Opcode error");
  }

}


void WasmVM::initialize_runtime_environment() {
  cache_linear_memory_layout();
  cache_table_layout();
  resolve_main_entrypoint();
  reset_runtime_state();
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
  // TODO: populate the globals once constant-expr evaluation is implemented.
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
