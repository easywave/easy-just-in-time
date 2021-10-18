
#include <benchmark/benchmark.h>
//#include <easy/code_cache.h>
#include <easy/jit.h>
#include <numeric>
#include <algorithm>
#include <functional>
#include <immintrin.h>
#include <math.h>

using namespace std::placeholders;

static void foo(void* dat) {
  (*(int*)dat) += 1;
}

static EASY_JIT_EXPOSE void foo2(void* dat) {
  (*(int*)dat) += 2;
}
using f_type = void(void*);
struct funcs{
  f_type *f;//[10];
  int size;
};
/*
static void map (void* data, unsigned nmemb, unsigned size, const funcs* func) {//void (*f)(void*)) {
  for(unsigned i = 0; i < nmemb; ++i)
    //for(int j = 0; j < func->size; ++j)
      (*func->f)((char*)data + i * size);
}
static void map (void* data, unsigned nmemb, unsigned size, void (*f)(void*)) {
  for(unsigned i = 0; i < nmemb; ++i)
    //for(int j = 0; j < func->size; ++j)
      f((char*)data + i * size);
}*/
static void map (void* data, unsigned nmemb, unsigned size, void(*f)(void*)) {
  for(unsigned i = 0; i < nmemb; ++i)
    //for(int j = 0; j < func->size; ++j)
      f((char*)data + i * size);
}
void test_func(benchmark::State& state) 
{
  // funcs func = {
  //   foo, //{foo, foo2},
  //   2};
  // easy::FunctionWrapper<void(void*, unsigned, unsigned)> map_w = easy::jit(map, _1, _2, _3, foo, easy::options::dump_ir("testfunc.ll"));
  auto foo_j = std::move(easy::jit(foo, _1));
  auto map_w = std::move(easy::jit(map, _1, _2, _3, foo_j, easy::options::dump_ir("testfunc.ll")));

  int data[] = {1,2,3,4};
  map_w(data, sizeof(data)/sizeof(data[0]), sizeof(data[0]));

  // CHECK: data[0] is 2
  // CHECK: data[1] is 3
  // CHECK: data[2] is 4
  // CHECK: data[3] is 5
  for(int v = 0; v != 4; ++v)
    printf("data[%d] is %d\n", v, data[v]);

}
//BENCHMARK(test_func);

static void loop1(int* data, int size)
{
  for (int i = 0; i < size; i++)
  {
    data[i] += 3;
  }
}
static void loop2(int* data, int size, void(*func)(int*,int))
{
  for (int i = 0; i < size; i++)
  {
    data[i] += 6;
  }
  func(data, size);
}
void test_func_same_loop(benchmark::State& state) 
{
  float pf[8] = {0};
    __m256 m;
    //__asm__("vmovups %1, %0" : "=x" (m) : "m" (*pf));
    __asm__("vmovups %0, %%ymm1" : /*"=x" (m)*/ : "m" (*pf) : "ymm1");
    __asm__(".byte 0x90" : : "x" (*pf) : "ymm0");
    __asm__(".byte 0x90" : : "x" (*pf) : "ymm2");
  auto foo_1 = easy::jit(loop1, _1, _2);
  auto foo_2 = easy::jit(loop2, _1, _2, foo_1, easy::options::dump_ir("loop1.ll"));
  int data[10000];
  foo_2(data, 10000);
}
//BENCHMARK(test_func_same_loop);
/* Horizontal add works within 128-bit lanes. Use scalar ops to add
 * across the boundary. */
static double reduce_vector1(__m256d input) {
  __m256d temp = _mm256_hadd_pd(input, input);
  return ((double*)&temp)[0] + ((double*)&temp)[2];
}

/* Another way to get around the 128-bit boundary: grab the first 128
 * bits, grab the lower 128 bits and then add them together with a 128
 * bit add instruction. */
static double reduce_vector2(__m256d input) {
  __m256d temp = _mm256_hadd_pd(input, input);
  __m128d sum_high = _mm256_extractf128_pd(temp, 1);
  __m128d result = _mm_add_pd(sum_high, _mm256_castpd256_pd128(temp));
  return ((double*)&result)[0];
}

double __attribute__((noinline)) dot_product(const double *a, const double *b, int N) 
{
  __m256d sum_vec = _mm256_set_pd(0.0, 0.0, 0.0, 0.0);
  //__m256i aa, b, c;
  //c = mm256_hadd_epi16(a, b);
  __m128i index = {0};

  /* Add up partial dot-products in blocks of 256 bits */
  for(int ii = 0; ii < N/4; ++ii) {
    
    __m256d x = _mm256_load_pd(a+4*ii);
    EMIT_STUB_FULL(x, "ymm15", "ymm14", "ymm13");
    //x = _mm256_i32gather_pd(a, index, 1);
    __m256d y = _mm256_load_pd(b+4*ii);
    __m256d z = _mm256_mul_pd(x,y);
    sum_vec = _mm256_add_pd(sum_vec, z);
  }

  /* Find the partial dot-product for the remaining elements after
   * dealing with all 256-bit blocks. */
  double final = 0.0;
  for(int ii = N-N%4; ii < N; ++ii)
    final += a[ii] * b[ii];

  return reduce_vector2(sum_vec) + final;
}

easy::FunctionWrapper<double(const double *a, const double *b)> my_kernel(nullptr);
static void kernel_avx2(benchmark::State& state) {
  using namespace std::placeholders;
  const int x = 2048;
  easy::RawBytes raw;
  raw.bytes = {0x90, 0x90, 0x90};
  raw.reserved_regs = {"ymm1", "xmm1", "ebx", "rax"};
  my_kernel = easy::jit_(raw, dot_product, _1, _2, x, easy::options::dump_ir("xxx.ll"),
    easy::options::opt_level(3, 0));
  __attribute__ ((aligned (32))) double a[x], b[x];
  for(int ii = 0; ii < x; ++ii)
    a[ii] = b[ii] = ii/sqrt(x);
  auto r = my_kernel(a, b);
  printf("result = %lf, org = %lf\n", r, dot_product(a, b, x));
}
BENCHMARK(kernel_avx2);

void __attribute__((noinline)) kernel(int n, int m, int * image, int const * mask, int* out) {
  for(int i = 0; i < n - m; ++i)
    for(int j = 0; j < n - m; ++j)
      for(int k = 0; k < m; ++k)
        for(int l = 0; l < m; ++l)
          out[i * (n-m+1) + j] += image[(i+k) * n + j+l] * mask[k *m + l];
}

/* To sort array elemets */

int int_cmp(int a, int b)
{
  if (a > b)
    return 1;
  else
  {
    if (a == b)
      return 0;
    else
      return -1;
  }
}

// https://github.com/ctasims/The-C-Programming-Language--Kernighan-and-Ritchie/blob/master/ch04-functions-and-program-structure/qsort.c
void __attribute__((noinline)) Qsort(int v[], int left, int right, int (*cmp)(int, int)) 
{
    int i, last;
    void swap(int v[], int i, int j);

    if (left >= right)  // do nothing if array contains < 2 elems
        return;
    // move partition elem to v[0]
    swap(v, left, (left + right)/2);
    last = left;

    for (i = left+1; i <= right; i++)  // partition
        if (cmp(v[i], v[left]))
            swap(v, ++last, i);

    swap(v, left, last);                // restore partition elem
    Qsort(v, left, last-1, cmp);
    Qsort(v, last+1, right, cmp);
}

/* swap: interchange v[i] and v[j] */
void swap(int v[], int i, int j)
{
    int temp;
    temp = v[i];
    v[i] = v[j];
    v[j] = temp;
}

static const int mask[3][3] = {{1,2,3},{0,0,0},{3,2,1}};

static void BM_convolve(benchmark::State& state) {
  using namespace std::placeholders;

  bool jit = state.range(1);
  int n = state.range(0);
  
  std::vector<int> image(n*n,0);
  std::vector<int> out((n-3)*(n-3),0);

  auto my_kernel = easy::jit(kernel, n, 3, _1, &mask[0][0], _2);

  benchmark::ClobberMemory();

  for (auto _ : state) {
    if(jit) {
      my_kernel(image.data(), out.data());
    } else {
      kernel(n, 3, image.data(), &mask[0][0], out.data());
    }
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_convolve)->Ranges({{16,1024}, {0,1}});

static void BM_convolve_compile_jit(benchmark::State& state) {
  using namespace std::placeholders;
  for (auto _ : state) {
    auto my_kernel = easy::jit(kernel, 11, 3, _1, &mask[0][0], _2);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_convolve_compile_jit);
/*
static void BM_convolve_cache_hit_jit(benchmark::State& state) {
  using namespace std::placeholders;
  static easy::Cache<> cache;
  cache.jit(kernel, 11, 3, _1, &mask[0][0], _2);
  benchmark::ClobberMemory();

  for (auto _ : state) {
    auto const &my_kernel = cache.jit(kernel, 11, 3, _1, &mask[0][0], _2);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_convolve_cache_hit_jit);

static void BM_qsort(benchmark::State& state) {
  using namespace std::placeholders;

  bool jit = state.range(1);
  int n = state.range(0);

  std::vector<int> vec(n);
  std::iota(vec.begin(), vec.end(), 0);
  std::random_shuffle(vec.begin(), vec.end());

  auto my_qsort = easy::jit(Qsort, _1, _2, _3, int_cmp);
  benchmark::ClobberMemory();

  for (auto _ : state) {
    if(jit) {
      my_qsort(vec.data(), 0, vec.size()-1);
    } else {
      Qsort(vec.data(), 0, vec.size()-1, int_cmp);
    }
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_qsort)->Ranges({{16,1024}, {0,1}});

static void BM_qsort_compile_jit(benchmark::State& state) {
  using namespace std::placeholders;
  for (auto _ : state) {
    auto my_qsort = easy::jit(Qsort, _1, _2, _3, int_cmp);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_qsort_compile_jit);

static void BM_qsort_cache_hit_jit(benchmark::State& state) {
  using namespace std::placeholders;
  static easy::Cache<> cache;
  cache.jit(Qsort, _1, _2, _3, int_cmp);
  benchmark::ClobberMemory();
  for (auto _ : state) {
    auto const &my_qsort = cache.jit(Qsort, _1, _2, _3, int_cmp);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_qsort_cache_hit_jit);
*/
BENCHMARK_MAIN();
