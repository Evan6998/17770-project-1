#include <algorithm>
#include <stdexcept>
#include <iostream>

#include "vm.h"

Value make_from(const std::string &s, wasm_type_t type) {
  Value v;
  v.type = type;
  switch (type)
  {
  case WASM_TYPE_I32:
    v.i32 = std::stoi(s);
    break;
  case WASM_TYPE_I64:
    v.i64 = std::stoll(s);
    break;
  case WASM_TYPE_F32:
    v.f32 = std::stof(s);
    break;
  case WASM_TYPE_F64:
    v.f64 = std::stod(s);
    break;
  default:
    throw std::runtime_error("Unsupported type in make_from");
  }
  return v;
}

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

  // TODO: invoke the main function body once the execution engine is ready.
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

  stack_.clear();
  prepare_globals_storage();
}

bool WasmVM::validate_main_signature(size_t argc) const {
  return main_ && main_->sig->params.size() == argc;
}

void WasmVM::push_main_arguments(const std::vector<std::string>& mainargs) {
  auto it = main_->sig->params.begin();
  for (size_t i = 0; i < mainargs.size(); i++) {
    auto type = *(it++);
    stack_.push_back(make_from(mainargs[i], type));
  }
}
