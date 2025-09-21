#pragma once 

#include "common.h"
#include "ir.h"

typedef struct {
  wasm_type_t type;
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    // std::shared_ptr<void> ref;
  };
} Value;

/* Create a Value from a string and type */
Value make_from(const std::string &s, wasm_type_t type);


class WasmVM {
public:
  WasmVM(const WasmModule& module);
  // default destructor is fine
  ~WasmVM() = default;

  void run(std::vector<std::string> mainargs);

private:
  void initialize_runtime_environment();
  void cache_linear_memory_layout();
  void cache_table_layout();
  void resolve_main_entrypoint();
  void prepare_globals_storage();
  void reset_runtime_state();
  bool validate_main_signature(size_t argc) const;
  void push_main_arguments(const std::vector<std::string>& mainargs);

  WasmModule module_;
  std::vector<byte> linear_memory_;
  std::vector<std::vector<FuncDecl*>> table_instances_;
  std::vector<Value> global_values_;
  std::vector<Value> stack_;
  std::vector<uint32_t> local_table_initial_sizes_;
  uint32_t initial_linear_memory_pages_ = 0;
  FuncDecl* main_ = nullptr;
};
