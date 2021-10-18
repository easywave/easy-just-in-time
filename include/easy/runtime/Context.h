#ifndef CONTEXT
#define CONTEXT

#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#include <easy/runtime/Function.h>

namespace easy {

struct serialized_arg {
  std::vector<char> buf;

  serialized_arg(char* serialized) {
    uint32_t size = *reinterpret_cast<uint32_t const*>(serialized);
    const char* data = serialized + sizeof(uint32_t);
    buf.insert(buf.end(), data, data+size);

    free(serialized);
  }
};

typedef void* layout_id;

struct ArgumentBase {

  enum ArgumentKind {
    AK_Forward,
    AK_Int,
    AK_Float,
    AK_Ptr,
    AK_Struct,
    AK_Module,
  };

  ArgumentBase() = default;
  virtual ~ArgumentBase() = default;

  bool operator==(ArgumentBase const &Other) const {
    return this->kind() == Other.kind() &&
        this->compareWithSameType(Other);
  }

  template<class ArgTy>
  std::enable_if_t<std::is_base_of<ArgumentBase, ArgTy>::value, ArgTy const*>
  as() const {
    if(kind() == ArgTy::Kind) return static_cast<ArgTy const*>(this);
    else return nullptr;
  }

  friend std::hash<easy::ArgumentBase>;

  virtual ArgumentKind kind() const noexcept = 0;

  protected:
  virtual bool compareWithSameType(ArgumentBase const&) const = 0;
  virtual size_t hash() const noexcept = 0;
};

#define DeclareArgument(Name, Type) \
  class Name##Argument \
    : public ArgumentBase { \
    \
    using HashType = std::remove_const_t<std::remove_reference_t<Type>>; \
    \
    Type Data_; \
    public: \
    Name##Argument(Type D) : ArgumentBase(), Data_(D) {} \
    virtual ~Name ## Argument() override = default ;\
    Type get() const { return Data_; } \
    static constexpr ArgumentKind Kind = AK_##Name;\
    ArgumentKind kind() const noexcept override  { return Kind; } \
    \
    protected: \
    bool compareWithSameType(ArgumentBase const& Other) const override { \
      auto const &OtherCast = static_cast<Name##Argument const&>(Other); \
      return Data_ == OtherCast.Data_; \
    } \
    \
    size_t hash() const noexcept override  { return std::hash<HashType>{}(Data_); } \
  }

DeclareArgument(Forward, unsigned);
DeclareArgument(Int, int64_t);
DeclareArgument(Float, double);
DeclareArgument(Ptr, void const*);
DeclareArgument(Module, easy::Function const&);

class StructArgument
    : public ArgumentBase {
  serialized_arg Data_;

  public:
  StructArgument(serialized_arg &&arg)
    : ArgumentBase(), Data_(arg) {}
  virtual ~StructArgument() override = default;
  std::vector<char> const & get() const { return Data_.buf; }
  static constexpr ArgumentKind Kind = AK_Struct;
  ArgumentKind kind() const noexcept override  { return Kind; }

  protected:
  bool compareWithSameType(ArgumentBase const& Other) const override {
    auto const &OtherCast = static_cast<StructArgument const&>(Other);
    return get() == OtherCast.get();
  }

  size_t hash() const noexcept override {
    std::hash<int64_t> hash{};
    size_t R = 0;
    for (char c : get())
      R ^= hash(c);
    return R;
  }
};

struct RawBytes {
  std::vector<uint8_t> bytes;
  std::vector<std::string> reserved_regs;
};

// class that holds information about the just-in-time context
class Context {

  std::vector<std::unique_ptr<ArgumentBase>> ArgumentMapping_;
  unsigned OptLevel_ = 2, OptSize_ = 0;
  std::string DebugFile_;

  // describes how the arguments of the function are passed
  //  struct arguments can be packed in a single int, or passed field by field,
  //  keep track of how many arguments a parameter takes
  std::vector<layout_id> ArgumentLayout_;

  template<class ArgTy, class ... Args>
  inline Context& setArg(Args && ... args) {
    ArgumentMapping_.emplace_back(new ArgTy(std::forward<Args>(args)...));
    return *this;
  }

  RawBytes RawBytes_;

  public:

  Context() = default;

  bool operator==(const Context&) const;
  
  // set the mapping between
  Context& setParameterIndex(unsigned);
  Context& setParameterInt(int64_t);
  Context& setParameterFloat(double);
  Context& setParameterPointer(void const*);
  Context& setParameterStruct(serialized_arg);
  Context& setParameterModule(easy::Function const&);

  Context& setArgumentLayout(layout_id id) {
    ArgumentLayout_.push_back(id); // each layout id is associated with a number of fields in the bitcode tracker
    return *this;
  }

  decltype(ArgumentLayout_) const & getLayout() const {
    return ArgumentLayout_;
  }

  template<class T>
  Context& setParameterTypedPointer(T* ptr) {
    return setParameterPointer(reinterpret_cast<const void*>(ptr));
  }

  Context& setOptLevel(unsigned OptLevel, unsigned OptSize) {
    OptLevel_ = OptLevel;
    OptSize_ = OptSize;
    return *this;
  }

  Context& setRawBytes(const RawBytes& raw) {
    RawBytes_ = raw;
    return *this;
  }

  Context& setDebugFile(std::string const &File) {
    DebugFile_ = File;
    return *this;
  }

  std::pair<unsigned, unsigned> getOptLevel() const {
    return std::make_pair(OptLevel_, OptSize_);
  }

  const RawBytes& getRawBytes() const {
    return RawBytes_;
  }

  std::string const& getDebugFile() const {
    return DebugFile_;
  }

  std::vector<std::unique_ptr<ArgumentBase>>::const_iterator begin() const { return ArgumentMapping_.begin(); }
  std::vector<std::unique_ptr<ArgumentBase>>::const_iterator end() const { return ArgumentMapping_.end(); }
  size_t size() const { return ArgumentMapping_.size(); }

  ArgumentBase const& getArgumentMapping(size_t i) const {
    return *ArgumentMapping_[i];
  }

  friend bool operator<(easy::Context const &C1, easy::Context const &C2);
}; 

}

namespace std
{
  template<class L, class R> struct hash<std::pair<L, R>>
  {
    typedef std::pair<L,R> argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& s) const noexcept {
      return std::hash<L>{}(s.first) ^ std::hash<R>{}(s.second);
    }
  };

  template<> struct hash<easy::ArgumentBase>
  {
    typedef easy::ArgumentBase argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& s) const noexcept {
      return s.hash();
    }
  };

  template<> struct hash<easy::Context>
  {
    typedef easy::Context argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& C) const noexcept {
      size_t H = 0;
      std::hash<easy::ArgumentBase> ArgHash;
      std::hash<std::pair<unsigned, unsigned>> OptHash;
      for(auto const &Arg : C)
        H ^= ArgHash(*Arg);
      H ^= OptHash(C.getOptLevel());
      return H;
    }
  };
}


#endif
