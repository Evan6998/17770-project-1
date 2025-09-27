#pragma once 

#include "common.h"
#include "ir.h"

#include <cstdint>
#include <stdexcept>
#include <variant>
#include <map>

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

// Runtime structures
struct Label {
  enum Kind { Implicit, Block, Loop, If } kind;
  const byte* pc_target;  // Loop: begin; Block/If: after end 
  const byte* pc_else;    // Only for If，otherwise nullptr
  size_t stack_height;    // operand stack height at entry
};

struct CtrlMeta {
  Label::Kind kind;
  // const byte* begin;   // address of the first instruction inside the block/loop/if
  const byte* else_pc; // only for if, else nullptr
  const byte* end;     // address of next instruction after end
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
  std::unordered_map<const byte*, CtrlMeta>  pre_indexing(FuncDecl* f);

private:
  void initialize_runtime_environment();
  void cache_linear_memory_layout();
  void cache_table_layout();
  void resolve_main_entrypoint();
  void prepare_globals_storage();
  void prepare_data_segments();
  void reset_runtime_state();
  bool validate_main_signature(size_t argc) const;
  void push_main_arguments(const std::vector<std::string>& mainargs);
  void skip_immediate(Opcode_t opcode, buffer_t &buf);

  bool invoke(FuncDecl* f);
  void run_op(buffer_t &buf, std::unordered_map<const byte*, CtrlMeta> &ctrl_map);
  void print_final_results();
  std::vector<Value> build_locals_for(const FuncDecl* f);

  inline void push(Value v) { operand_stack_.push_back(v); }
  inline Value top() {
    if (operand_stack_.empty()) {
      throw std::runtime_error("operand stack underflow");
    }
    return operand_stack_.back();
  }
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
