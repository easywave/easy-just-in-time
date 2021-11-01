#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Host.h> 
#include <llvm/Target/TargetMachine.h> 
#include <llvm/Support/TargetRegistry.h> 
#include <llvm/Analysis/TargetTransformInfo.h> 
#include <llvm/Analysis/TargetLibraryInfo.h> 
#include <llvm/Support/FileSystem.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/CodeGen/MachineRegisterInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Linker/Linker.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Transforms/Scalar.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <algorithm>

#define handle_error(msg) \
    do { perror(msg); exit(EXIT_FAILURE); } while (0)

#ifdef NDEBUG
#include <llvm/IR/Verifier.h>
#endif


using namespace easy;
char ReserveReg::ID = 0;

static llvm::RegisterPass<ReserveReg> x("RegisterPass", "RegisterPass", false, false);
static void registerHelloMFPass(const llvm::PassManagerBuilder &,
                        llvm::legacy::PassManagerBase &PM) {
  PM.add(new ReserveReg());
}

static llvm::RegisterStandardPasses
  z(llvm::PassManagerBuilder::EP_EarlyAsPossible, 
                 registerHelloMFPass);

bool ReserveReg::runOnMachineFunction(llvm::MachineFunction& Fn) {
  llvm::MachineRegisterInfo& reg_info = Fn.getRegInfo();
  const llvm::TargetMachine & target_machine = Fn.getTarget();

  auto TRI = Fn.getSubtarget().getRegisterInfo();
  llvm::BitVector ReservedRegs = TRI->getReservedRegs(Fn);
  reg_info.getReservedRegs();

  llvm::errs() << "I saw a function called " << Fn.getName() << "!\n";
  return false;
}
namespace easy {
  DefineEasyException(ExecutionEngineCreateError, "Failed to create execution engine for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

static std::unique_ptr<llvm::TargetMachine> GetHostTargetMachine() {
  std::unique_ptr<llvm::TargetMachine> TM(llvm::EngineBuilder().selectTarget());
  return TM;
}

struct FakeTargetPassConfig : public llvm::TargetPassConfig
{
  using llvm::TargetPassConfig::addPass;
  virtual void addPreRegAlloc() { }
  FakeTargetPassConfig(llvm::LLVMTargetMachine &TM, llvm::PassManagerBase &pm) :
    llvm::TargetPassConfig(TM, pm) {}
};
union addPassType {
  void(*addr)(void*, void*, bool, bool);
  void(FakeTargetPassConfig::*func)(llvm::Pass*, bool, bool);
};
addPassType g_addPass;
union addPreRegType {
  int64_t addr;
  void(FakeTargetPassConfig::*func)();
  void (*generic_func)(void*);
};
addPreRegType g_org;
void new_addPreRegAlloc(void* pThis)
{
  g_org.generic_func(pThis);
  g_addPass.addr(pThis, new ReserveReg(), true, true);
}

union getReservedRegsType {
  int64_t addr;
  llvm::BitVector(llvm::TargetRegisterInfo::*func)(const llvm::MachineFunction &MF) const;
  llvm::BitVector (*generic_func)(void*, const llvm::MachineFunction &MF);
};
static getReservedRegsType g_org_getReservedRegsType;
thread_local const std::vector<std::string>* g_reservedRegs;
// TODO: add lock
static std::map<std::string, unsigned int> g_regNameToIdx;

std::string to_upper(const std::string s) {
  std::string result = s;
  std::transform(s.begin(), s.end(), result.begin(), ::toupper);
  return result;
}
// change the reserved the register list
llvm::BitVector Hooked_getReservedRegsType(void* pThis, const llvm::MachineFunction &MF) {
  if (g_regNameToIdx.empty()) {
    llvm::TargetRegisterInfo* p = (llvm::TargetRegisterInfo*)pThis;
    for (unsigned int i = 0; i < p->getNumRegs(); i++)
    {
      auto reg = to_upper(std::string(p->getName(i)));
      //printf("%d %s\n", i, reg.c_str());
      g_regNameToIdx[reg] = i;
    }
  }
  
  const auto& name = MF.getName();
  auto bit = g_org_getReservedRegsType.generic_func(pThis, MF);
  for (int i = 0; i < g_reservedRegs->size(); i++) {
    auto reg = to_upper((*g_reservedRegs)[i]);
    auto idx = g_regNameToIdx[reg];
    bit.set(idx);
  }
  
  return bit;
}

static void Hook_getReservedRegs(llvm::Module& M, llvm::LLVMTargetMachine* llvmTM, const char* Name) {
  if (!g_org_getReservedRegsType.addr) {
    auto func = M.getFunction(Name);
    auto TRI = llvmTM->getSubtargetImpl(*func)->getRegisterInfo();
    auto getReservedRegs_addr = &llvm::TargetRegisterInfo::getReservedRegs;
    getReservedRegsType dumy_getReservedRegs;
    dumy_getReservedRegs.func = getReservedRegs_addr;
    
    // for virtual function the layout: https://blog.mozilla.org/nfroyd/2014/02/20/finding-addresses-of-virtual-functions/
    int idxInVtable = (dumy_getReservedRegs.addr - 1) / sizeof(void*);
    auto vtbl = (int64_t*)(*((int64_t*)TRI));
    const auto PAGE_SIZE = (uint64_t)sysconf(_SC_PAGESIZE);
    auto aliged_addr = (void*)((uint64_t)vtbl & ~(PAGE_SIZE - 1));
    if (mprotect(aliged_addr, PAGE_SIZE, PROT_READ|PROT_WRITE) == -1)
      handle_error("mprotect");

    g_org_getReservedRegsType.addr = vtbl[idxInVtable];
    getReservedRegsType hooked;
    hooked.generic_func = Hooked_getReservedRegsType;
    vtbl[idxInVtable] = hooked.addr;
  }
}

static void Optimize(llvm::Module& M, const char* Name, const easy::Context& C, unsigned OptLevel, unsigned OptSize) {

  llvm::Triple Triple{llvm::sys::getProcessTriple()};

  llvm::PassManagerBuilder Builder;
  Builder.OptLevel = OptLevel;
  Builder.SizeLevel = OptSize;
  Builder.LibraryInfo = new llvm::TargetLibraryInfoImpl(Triple);
  Builder.Inliner = llvm::createFunctionInliningPass(OptLevel, OptSize, false);

  std::unique_ptr<llvm::TargetMachine> TM = GetHostTargetMachine();
  assert(TM);
  TM->adjustPassManager(Builder);

  llvm::legacy::PassManager MPM;
  MPM.add(llvm::createTargetTransformInfoWrapperPass(TM->getTargetIRAnalysis()));
  MPM.add(easy::createContextAnalysisPass(C));
  MPM.add(easy::createEmitCodesPass(Name));
  MPM.add(easy::createInlineParametersPass(Name));
  Builder.populateModulePassManager(MPM);
  //MPM.add(easy::createDevirtualizeConstantPass(Name));
  //MPM.add(llvm::createConstantPropagationPass());
  //MPM.add(llvm::createSCCPPass());    // Unroll small loops
  //MPM.add(llvm::createLoopUnrollPass());    // Unroll small loops
  //MPM.add(llvm::createSimpleLoopUnrollPass());    // Unroll small loops

  // LoopUnroll may generate some redundency to cleanup.
  //MPM.add(llvm::createInstructionCombiningPass());
  //MPM.add(llvm::createLoopInstSimplifyPass());

  // Runtime unrolling will introduce runtime check in loop prologue. If the
  // unrolled loop is a inner loop, then the prologue will be inside the
  // outer loop. LICM pass can help to promote the runtime check out if the
  // checked value is loop invariant.
  //MPM.add(llvm::createLICMPass());
  //MPM.add(easy::createIndirectcallConstantPass(Name));

  llvm::LLVMTargetMachine* llvmTM = static_cast<llvm::LLVMTargetMachine*>(TM.get());

  // the following trick was affected by https://github.com/jmpews/NoteZ/issues/47
  // hook TargetRegisterInfo::getReservedRegs
  Hook_getReservedRegs(M, llvmTM, Name);

#if 0
  // remain for reference
  // get TargetPassConfig::addPass address
  //https://stackoverflow.com/questions/3053561/how-do-i-assign-an-alias-to-a-function-name-in-c
  auto addPass_addr = static_cast<void(FakeTargetPassConfig::*)(llvm::Pass*, bool, bool)>(&FakeTargetPassConfig::addPass);
  g_addPass.func = addPass_addr;

  // hook X86TargetConfig::addPreRegAlloc
  auto config = llvmTM->createPassConfig(MPM);
  FakeTargetPassConfig fake_config(*llvmTM, MPM);
  auto vtbl = (int64_t*)(*((int64_t*)&fake_config));
  auto addPreRegAlloc_addr = static_cast<void(FakeTargetPassConfig::*)()>(&FakeTargetPassConfig::addPreRegAlloc);
  addPreRegType dummy_addPreReg;
  dummy_addPreReg.func = addPreRegAlloc_addr;
  // for virtual function the layout: https://blog.mozilla.org/nfroyd/2014/02/20/finding-addresses-of-virtual-functions/
  int idxInVtable = (dummy_addPreReg.addr - 1) / sizeof(void*);
  auto vtbl_x86 = (int64_t*)(*((int64_t*)config));
  const auto PAGE_SIZE = (uint64_t)sysconf(_SC_PAGESIZE);
  auto aliged_addr = (void*)((uint64_t)vtbl_x86 & ~(PAGE_SIZE - 1));
  if (mprotect(aliged_addr, PAGE_SIZE, PROT_READ|PROT_WRITE) == -1)
    handle_error("mprotect");
  g_org.addr = vtbl_x86[idxInVtable];
  addPreRegType hooked;
  hooked.generic_func = new_addPreRegAlloc;
  vtbl_x86[idxInVtable] = hooked.addr;
#endif

#ifdef NDEBUG
  MPM.add(llvm::createVerifierPass());
#endif

  Builder.populateModulePassManager(MPM);

  MPM.run(M);
}

static std::unique_ptr<llvm::ExecutionEngine> GetEngine(std::unique_ptr<llvm::Module> M, const char *Name) {
  llvm::EngineBuilder ebuilder(std::move(M));
  std::string eeError;

  std::unique_ptr<llvm::ExecutionEngine> EE(ebuilder.setErrorStr(&eeError)
          .setMCPU(llvm::sys::getHostCPUName())
          .setEngineKind(llvm::EngineKind::JIT)
          .setOptLevel(llvm::CodeGenOpt::Level::Aggressive)
          .create());

  if(!EE) {
    throw easy::ExecutionEngineCreateError(Name);
  }

  return EE;
}

static void MapGlobals(llvm::ExecutionEngine& EE, GlobalMapping* Globals) {
  for(GlobalMapping *GM = Globals; GM->Name; ++GM) {
    EE.addGlobalMapping(GM->Name, (uint64_t)GM->Address);
  }
}

static void WriteOptimizedToFile(llvm::Module const &M, std::string const& File) {
  if(File.empty())
    return;
  std::error_code Error;
  llvm::raw_fd_ostream Out(File, Error, llvm::sys::fs::F_None);

  if(Error)
    throw CouldNotOpenFile(Error.message());

  Out << M;
}

std::unique_ptr<Function>
CompileAndWrap(const char*Name, GlobalMapping* Globals,
               std::unique_ptr<llvm::LLVMContext> Ctx,
               std::unique_ptr<llvm::Module> M) {

  llvm::Module* MPtr = M.get();
  std::unique_ptr<llvm::ExecutionEngine> EE = GetEngine(std::move(M), Name);

  if(Globals) {
    MapGlobals(*EE, Globals);
  }

  void *Address = (void*)EE->getFunctionAddress(Name);

  std::unique_ptr<LLVMHolder> Holder(new easy::LLVMHolderImpl{std::move(EE), std::move(Ctx), MPtr});
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}

static void LinkPointerIfPossible(llvm::Module &M, const std::string &Name) {
  auto &BT = easy::BitcodeTracker::GetTracker();
  auto Ptr = BT.getAddress(Name);
  if(Ptr) {
    std::unique_ptr<llvm::Module> LM = BT.getModuleWithContext(Ptr, M.getContext());

    if(!llvm::Linker::linkModules(M, std::move(LM), llvm::Linker::OverrideFromSrc,
                            [](llvm::Module &, const llvm::StringSet<> &){}))
    {
      llvm::GlobalValue *GV = M.getNamedValue(Name);
      if(auto* G = llvm::dyn_cast<llvm::GlobalVariable>(GV)) {
        GV->setLinkage(llvm::Function::PrivateLinkage);
      }
      else if(llvm::Function* F = llvm::dyn_cast<llvm::Function>(GV)) {
        F->setLinkage(llvm::Function::PrivateLinkage);
      }
      //assert(false && "wtf");
    }
  }
}

static void ResolveModule(llvm::Module& M, const llvm::StringRef& Name) {
  auto& F = *M.getFunction(Name);
  std::vector<std::string> ToDo;
  for(auto it = llvm::inst_begin(F); it != inst_end(F); ++it) {
    llvm::CallSite CS{&*it};
    if(!CS)
      continue;

    llvm::Function* Called = CS.getCalledFunction();
    if (Called == nullptr)
      continue;
    auto CalledName = Called->getName();
    if (CalledName.empty())
      continue;
    ToDo.push_back(CalledName);
    //LinkPointerIfPossible(M, CalledName);
  }
  for (auto &&N : ToDo) {
    LinkPointerIfPossible(M, N);
  }
}

llvm::Module const& Function::getLLVMModule() const {
  return *static_cast<LLVMHolderImpl const&>(*this->Holder).M_;
}

std::unique_ptr<Function> Function::Compile(void *Addr, easy::Context const& C) {

  auto &BT = BitcodeTracker::GetTracker();

  const char* Name;
  GlobalMapping* Globals;
  std::tie(Name, Globals) = BT.getNameAndGlobalMapping(Addr);

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::tie(M, Ctx) = BT.getModule(Addr);

  ResolveModule(*M, Name);

  unsigned OptLevel;
  unsigned OptSize;
  std::tie(OptLevel, OptSize) = C.getOptLevel();

  Optimize(*M, Name, C, OptLevel, OptSize);

  WriteOptimizedToFile(*M, C.getDebugFile());

  const auto& raw = C.getRawBytes();
  g_reservedRegs = &raw.reserved_regs;
  auto f = CompileAndWrap(Name, Globals, std::move(Ctx), std::move(M));
  return f;
}

void easy::Function::serialize(std::ostream& os) const {
  std::string buf;
  llvm::raw_string_ostream stream(buf);

  LLVMHolderImpl const *H = reinterpret_cast<LLVMHolderImpl const*>(Holder.get());
  llvm::WriteBitcodeToFile(H->M_, stream);
  stream.flush();

  os << buf;
}

std::unique_ptr<easy::Function> easy::Function::deserialize(std::istream& is) {

  auto &BT = BitcodeTracker::GetTracker();

  std::string buf(std::istreambuf_iterator<char>(is), {}); // read the entire istream
  auto MemBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(buf));

  std::unique_ptr<llvm::LLVMContext> Ctx(new llvm::LLVMContext());
  auto ModuleOrError = llvm::parseBitcodeFile(*MemBuf, *Ctx);
  if(ModuleOrError.takeError()) {
    return nullptr;
  }

  auto M = std::move(ModuleOrError.get());

  std::string FunName = easy::GetEntryFunctionName(*M);

  GlobalMapping* Globals = nullptr;
  if(void* OrigFunPtr = BT.getAddress(FunName)) {
    std::tie(std::ignore, Globals) = BT.getNameAndGlobalMapping(OrigFunPtr);
  }

  return CompileAndWrap(FunName.c_str(), Globals, std::move(Ctx), std::move(M));
}

bool Function::operator==(easy::Function const& other) const {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*this->Holder);
  LLVMHolderImpl& Other = static_cast<LLVMHolderImpl&>(*other.Holder);
  return This.M_ == Other.M_;
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*F.Holder);
  return std::hash<llvm::Module*>{}(This.M_);
}
