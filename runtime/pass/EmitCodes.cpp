#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/BitcodeTracker.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Linker/Linker.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/raw_ostream.h>
#include <numeric>
#include <sstream>

using namespace llvm;

char easy::EmitCodes::ID = 0;

llvm::Pass* easy::createEmitCodesPass(llvm::StringRef Name) {
  return new EmitCodes(Name);
}

static std::string BuildEmitString(const std::vector<uint8_t>& src) {
  if (src.empty())
    return "";

  std::ostringstream ss;
  for (auto i = 0; i < src.size(); i++)
  {
    ss << ".byte " << std::to_string(src[i]) << ";";
  }
  
  return ss.str();
}

bool easy::EmitCodes::runOnFunction(llvm::Function &F) {
  if(F.getName() != TargetName_)
    return false;

  easy::Context const &C = getAnalysis<ContextAnalysis>().getContext();
  const auto &RawBytes = C.getRawBytes();
  std::string s = BuildEmitString(RawBytes.bytes);
  bool changed = false;
  std::list<llvm::Instruction*> ToRemove;
  for(auto it = inst_begin(F); it != inst_end(F); ++it) {
    CallSite CS{&*it};
    if(!CS)
      continue;

    Value* Called = CS.getCalledValue();
    if (!isa<InlineAsm>(Called))
      continue;
    //if (s.empty()) {
    //  changed = true;
    //  ToRemove.push_back(&*it);
    //  continue;
    //}
    changed = true;
    const InlineAsm *IA = cast<InlineAsm>(Called);
    InlineAsm *newIA = InlineAsm::get(
      IA->getFunctionType(),
      s,
      IA->getConstraintString(),
      s.empty() ? false : IA->hasSideEffects(),
      IA->isAlignStack(),
      IA->getDialect());
    CS.setCalledFunction(newIA);
  }

  //for (auto &&i : ToRemove) {
  //  i->eraseFromParent();
  //}
  
  return changed;
}

static RegisterPass<easy::EmitCodes> X("","",false, false);
