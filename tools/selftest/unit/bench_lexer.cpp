// ============================================================================
// bench_lexer.cpp —— 词法器吞吐量基准（进程内循环，**不含进程启动开销**）
//
// 为什么需要它：`perf_tokens.py` 测的是"比赛口径"（每次调用一个进程），
// 其中 ~1.6 ms/文件 是动态链接 + main 启动的固定成本，**掩盖了算法本身的性能**。
// 这个基准把源文件读进内存后反复 token 化，直接给出：
//
//   * 每轮耗时（最好 / 中位）
//   * MB/s 与 token/s 吞吐
//
// 这才是"扫描是否 O(n)、有没有退化成 O(n²)"的证据。
//
// 用法: bench_lexer <file.sy> [轮数=200]
// 依赖: run.sh（不需要第三方测试框架）
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "frontend/Lexer.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::DiagnosticEngine;
using sysy::Lexer;
using sysy::SourceFile;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "用法: bench_lexer <file.sy> [轮数=200]\n";
    return 2;
  }
  const std::string path = argv[1];
  const int rounds = argc >= 3 ? std::atoi(argv[2]) : 200;
  if (rounds <= 0) {
    std::cerr << "轮数必须 > 0\n";
    return 2;
  }

  SourceFile src = SourceFile::load(path);
  const size_t bytes = src.text().size();

  std::vector<double> ms;
  ms.reserve(static_cast<size_t>(rounds));
  size_t nTokens = 0;
  size_t errors = 0;

  for (int r = 0; r < rounds; ++r) {
    DiagnosticEngine diags;              // 每轮清空，避免诊断累积
    diags.setSourceFile(&src);
    Lexer lx(src, diags);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<sysy::Token> toks = lx.tokenize();
    const auto t1 = std::chrono::steady_clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    nTokens = toks.size();
    errors = lx.errorCount();
  }

  std::vector<double> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  const double best = sorted.front();
  const double med = sorted[sorted.size() / 2];
  const double worst = sorted.back();

  std::printf("文件      : %s\n", path.c_str());
  std::printf("字节数    : %zu\n", bytes);
  std::printf("token 数  : %zu（词法错误 %zu）\n", nTokens, errors);
  std::printf("轮数      : %d\n", rounds);
  std::printf("最好      : %.3f ms   (%.1f MB/s, %.2f M token/s)\n", best,
              bytes / (best / 1000.0) / (1024.0 * 1024.0),
              nTokens / (best / 1000.0) / 1e6);
  std::printf("中位      : %.3f ms   (%.1f MB/s, %.2f M token/s)\n", med,
              bytes / (med / 1000.0) / (1024.0 * 1024.0),
              nTokens / (med / 1000.0) / 1e6);
  std::printf("最差      : %.3f ms\n", worst);
  return 0;
}
