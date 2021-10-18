#ifndef EASY
#define EASY

#include <easy/runtime/Context.h>
#include <easy/attributes.h>
#include <easy/param.h>
#include <easy/function_wrapper.h>

#include <memory>
#include <type_traits>
#include <tuple>
#include <cassert>

namespace easy {

namespace {
template<class Ret, class ... Params>
FunctionWrapper<Ret(Params ...)>
WrapFunction(std::unique_ptr<Function> F, meta::type_list<Ret, Params ...>) {
  return FunctionWrapper<Ret(Params ...)>(std::move(F));
}

template<typename T>
using _FunOriginalTy = std::remove_pointer_t<std::decay_t<T>>;
template<class T, class ... Args>
using _new_type_traits = meta::new_function_traits<_FunOriginalTy<T>, meta::type_list<Args...>>;
template<class T, class ... Args>
using _new_return_type = typename _new_type_traits<T, Args...>::return_type;
template<class T, class ... Args>
using _new_parameter_types = typename _new_type_traits<T, Args...>::parameter_list;
template<class T, class ... Args>
using FuncType = decltype(WrapFunction(0, 
                            typename _new_parameter_types<T, Args...>::template 
                            push_front<_new_return_type<T, Args...>> ()));

template<class T, class ... Args>
FuncType<T, Args...>
jit_with_context(easy::Context const &C, T &&Fun) {

  auto* FunPtr = meta::get_as_pointer(Fun);
  using FunOriginalTy = std::remove_pointer_t<std::decay_t<T>>;

  using new_type_traits = meta::new_function_traits<FunOriginalTy, meta::type_list<Args...>>;
  using new_return_type = typename new_type_traits::return_type;
  using new_parameter_types = typename new_type_traits::parameter_list;

  auto CompiledFunction =
      Function::Compile(reinterpret_cast<void*>(FunPtr), C);

  auto Wrapper =
      WrapFunction(std::move(CompiledFunction),
                   typename new_parameter_types::template push_front<new_return_type> ());
  return Wrapper;
}

template<class T, class ... Args>
easy::Context get_context_for(Args&& ... args) {
  using FunOriginalTy = std::remove_pointer_t<std::decay_t<T>>;
  static_assert(std::is_function<FunOriginalTy>::value,
                "easy::jit: supports only on functions and function pointers");

  using parameter_list = typename meta::function_traits<FunOriginalTy>::parameter_list;

  static_assert(parameter_list::size <= sizeof...(Args),
                "easy::jit: not providing enough argument to actual call");

  easy::Context C;
  easy::set_parameters<parameter_list, Args&&...>(parameter_list(), C,
                                                  std::forward<Args>(args)...);
  return C;
}

template<class T, class ... Args>
easy::Context get_context_for_(const RawBytes& rawBytes, Args&& ... args) {
  using FunOriginalTy = std::remove_pointer_t<std::decay_t<T>>;
  static_assert(std::is_function<FunOriginalTy>::value,
                "easy::jit: supports only on functions and function pointers");

  using parameter_list = typename meta::function_traits<FunOriginalTy>::parameter_list;

  static_assert(parameter_list::size <= sizeof...(Args),
                "easy::jit: not providing enough argument to actual call");

  easy::Context C;
  C.setRawBytes(rawBytes);
  easy::set_parameters<parameter_list, Args&&...>(parameter_list(), C,
                                                  std::forward<Args>(args)...);
  return C;
}
}

template<class T, class ... Args>
FuncType<T, Args...>
EASY_JIT_COMPILER_INTERFACE jit(T &&Fun, Args&& ... args) {
  auto C = get_context_for<T, Args...>(std::forward<Args>(args)...);
  return jit_with_context<T, Args...>(C, std::forward<T>(Fun));
}

template<class T, class ... Args>
FuncType<T, Args...>
EASY_JIT_COMPILER_INTERFACE jit_(const RawBytes& rawBytes, T &&Fun, Args&& ... args) {
  auto C = get_context_for_<T, Args...>(rawBytes, std::forward<Args>(args)...);
  return jit_with_context<T, Args...>(C, std::forward<T>(Fun));
}

// https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html
// https://stackoverflow.com/questions/52014436/using-a-specific-zmm-register-in-inline-asm
// https://blog.csdn.net/lwx62/article/details/82796364
// output_var: store the result to 'output_var'(offen c++ variable)
// output_reg: the result source in which register
// __m512 xxx() {
//     __m512 x;
//     EMIT_STUB_FULL(x, "zmm13", "zmm14");
//     return x;
// }
#define EMIT_STUB_FULL(input_var, input_reg, ...) \
  {register decltype(input_var) __input_var__ asm(input_reg) = input_var; \
  __asm__ __volatile__(\
        ".byte 0x90\n\t"                :   \
        "=v" (__input_var__)            : /* outputs */\
        "0" (__input_var__)             : /* inputs */ \
        __VA_ARGS__);                     /* modify */\
  input_var = __input_var__;}

#define EMIT_STUB_NOMODIFY(input_var, input_reg) \
  {register decltype(input_var) __input_var__ asm(input_reg) = input_var; \
  __asm__ __volatile__(\
        ".byte 0x90\n\t"                :   \
        "=v" (__input_var__)            : /* outputs */\
        "0" (__input_var__));             /* inputs */ \
  input_var = __input_var__;}

}
#endif
