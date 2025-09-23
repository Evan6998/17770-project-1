#include <algorithm>
#include <stdexcept>
#include <iostream>

#include "vm.h"

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
  size_t result_count = main_->sig->results.size();
  if (result_count == 0) {
    return;
  }
  if (sp() != result_count) {
    throw std::runtime_error("Operand stack size does not match expected result count");
  }

  main_->sig->results.reverse();
  // Print results in order
  for (auto it = main_->sig->results.begin(); it != main_->sig->results.end(); ++it) {
    TRACE("Result type: %s\n", wasm_type_string(*it));
    Value value = pop();
    if ((*it) == WASM_TYPE_F64) {
      double v = std::get<double>(value);
      std::cout.precision(6);
      std::cout << std::fixed << v << std::endl;
    } else {
      // print other types directly
      std::visit([](auto&& arg) { std::cout << arg << std::endl; }, value);
    }
  }
}

std::vector<Value> WasmVM::build_locals_for(const FuncDecl* f) {
  std::vector<Value> locals;
  locals.reserve(f->sig->params.size() + f->num_pure_locals);
  // Add parameters from the operand stack
  size_t param_count = f->sig->params.size();
  if (sp() < param_count) {
    throw std::runtime_error("Not enough values on the operand stack for function parameters");
  }

  for (size_t i = 0; i < param_count; ++i) {
    locals.push_back(pop());
  }
  std::reverse(locals.begin(), locals.begin() + param_count);
  return locals;
}

bool WasmVM::invoke(FuncDecl* f) {
  byte* start = f->code_bytes.data();
  byte* end = start + f->code_bytes.size();
  
  Frame frame = {
    .func = f,
    .locals = build_locals_for(f),
    .pc = {start, start, end},
    .labels = {},
    .stack_height_on_entry = sp()
  };

  // push an implicit label for the function body
  frame.labels.push_back(Label {
    .pc_begin = start,
    .pc_end = end,
    .pc_else = nullptr,
    .stack_height = sp(),
    .kind = Label::Kind::Block
  });

  // trace local vector values:
  TRACE("Invoking function with %lu locals\n", frame.locals.size());
  for (size_t i = 0; i < frame.locals.size(); ++i) {
    std::visit([i](auto&& arg) { TRACE("  local[%lu]: %s\n", i, std::to_string(arg).c_str()); }, frame.locals[i]);
  }

  call_stack_.push_back(frame);

  // TODO: implement the function body execution
  while (!call_stack_.empty()) {
    auto& current_frame = call_stack_.back();
    run_op(current_frame.pc);
  }
  return true;
}

void WasmVM::run_op(buffer_t &buf) {
  const byte* startp = buf.ptr;

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
      auto v = static_cast<float>(RD_U32_RAW());
      TRACE("F32_CONST: %f\n", v);
      push(v);
      break;
    }
    case WASM_OP_F64_CONST: {
      auto v = static_cast<double>(RD_U64_RAW());
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
      TRACE("LOCAL_GET: index %u value ", local_idx);
      std::visit([](auto&& arg) { TRACE("%s\n", std::to_string(arg).c_str()); }, local_value);
      push(local_value);
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
    case WASM_OP_UNREACHABLE: {
      throw std::runtime_error("unreachable executed");
      break;
    }
    case WASM_OP_END: {
      // End of a block, loop, if, or function
      Frame& current_frame = call_stack_.back();
      auto block = current_frame.labels.back();
      current_frame.labels.pop_back();

      if (current_frame.labels.empty()) {
        auto last_frame = call_stack_.back();
        call_stack_.pop_back();

        auto return_count = last_frame.func->sig->results.size();
        if (sp() < return_count) {
          throw std::runtime_error("Not enough values on the operand stack for function return");
        }
        // Save return values
        std::vector<Value> return_values;
        for (size_t i = 0; i < return_count; ++i) {
          return_values.push_back(pop());
        }
        std::reverse(return_values.begin(), return_values.end());
        // Restore operand stack to state before function call

        pop_to(last_frame.stack_height_on_entry);
        // push return values if any
        for (auto result : return_values) {
          push(result);
        }
        TRACE("Function return with %lu values\n", return_values.size());
      }
      break;
    }
    case WASM_OP_DROP: {
      if (sp() < 1) {
        throw std::runtime_error("Not enough values on the operand stack for drop");
      }
      Value dropped = pop();
      TRACE("DROP: ");
      std::visit([](auto&& arg) { TRACE("%s\n", std::to_string(arg).c_str()); }, dropped);
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
      TRACE("SELECT: condition %d, selected ", std::get<std::int32_t>(condition));
      std::visit([](auto&& arg) { TRACE("%s\n", std::to_string(arg).c_str()); }, selected);
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
