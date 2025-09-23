#pragma once 

#include "common.h"
#include "ir.h"

#include <cstdint>
#include <stdexcept>
#include <variant>

using Value = std::variant<
  std::int32_t,      // i32  (0x7F)
  std::int64_t,      // i64  (0x7E)
  float,             // f32  (0x7D)
  double            // f64  (0x7C)
>;

/* Create a Value from a string and type */
inline Value make_from(const std::string &s, wasm_type_t type) {
  switch (type) {
    case WASM_TYPE_I32:
      return std::stoi(s);
    case WASM_TYPE_I64:
      return std::stoll(s);
    case WASM_TYPE_F32:
      return std::stof(s);
    case WASM_TYPE_F64:
      return std::stod(s);
    default:
      throw std::runtime_error("Unsupported type for make_from");
  }
}

struct Label {
  enum Kind { Block, Loop, If } kind;
  const byte* pc_begin;   // first instr inside the region (after header)
  const byte* pc_end;     // first instr after matching 'end'
  const byte* pc_else;    // first instr after 'else' (nullptr if no else / not-if)
  size_t stack_height;    // operand stack height at entry
  // uint32_t result_arity;  // only 0 in this project
  // bool is_loop;           // determines where 'br' goes
};


struct Frame {
  FuncDecl* func;
  buffer_t pc;
  std::vector<Value> locals;
  std::vector<Label> labels;
  // to restore on return
  size_t stack_height_on_entry;
};

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

  bool invoke(FuncDecl* f);
  void run_op(buffer_t &buf);
  void print_final_results();
  std::vector<Value> build_locals_for(const FuncDecl* f);

  inline void push(Value v) { operand_stack_.push_back(v); }
  inline Value pop() {
    if (operand_stack_.empty()) {
      throw std::runtime_error("operand stack underflow");
    }
    Value v = operand_stack_.back();
    operand_stack_.pop_back();
    return v;
  }
  inline void pop_to(size_t h) { operand_stack_.resize(h); }
  inline size_t sp() const { return operand_stack_.size(); }

  WasmModule module_;
  std::vector<byte> linear_memory_;
  std::vector<std::vector<FuncDecl*>> table_instances_;
  std::vector<Value> global_values_;
  std::vector<Value> operand_stack_;
  std::vector<Frame> call_stack_;
  std::vector<uint32_t> local_table_initial_sizes_;
  uint32_t initial_linear_memory_pages_ = 0;
  FuncDecl* main_ = nullptr;
};
