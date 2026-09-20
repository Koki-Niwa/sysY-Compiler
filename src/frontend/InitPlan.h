// ============================================================================
// InitPlan —— **初始化计划**：纯数据，无逻辑（S04 交付物 #1）
//
// 这是 `InitLowering` 的产物、S05（IRGen）的输入。prompt §四.1 把它冻结成
// 一个纯数据结构 —— 本文件**不许**出现任何计算、查询或遍历函数：
// 一旦这里长出"顺手算一下大小"这类方法，它就会变成第二个真相的温床。
//
// ── 为什么需要"计划"这一层（prompt §二）───────────────────────────────────
//   前三关的产物都能靠"形状"判对错，初始化**不能**：同一棵 `InitVal` 树既能
//   降级成 1 条动作、也能降级成 4096 条，**两者语义都对，只有一个能过性能关**。
//   语料里有 6 个文件声明了 `int x[600][600][600]`（各 864 MB）。所以计划把
//   "语义"与"规模"分开表达：
//     * 全局对象：要么"整片为零"，要么只列**非零元素**（默认值 + 稀疏覆盖，
//       与 `ConstObject` 是同一种表示 —— prompt §一 的第 3 条硬约束）
//     * 局部对象：一串**按执行顺序**的动作，零填充可以是一条 `Zero`
//
// ── 两处刻意的表示决定（都在报告里交代过）────────────────────────────────
//   ① `offset` 一律是**相对对象起始的字节偏移**（不是元素下标）。
//      理由：S05 要按字节地址发射 `getelementptr`/`store`；若这里给元素下标，
//      IRGen 就得知道元素宽度 —— 那是把"类型的字节大小"这条知识复制一份。
//   ② 全局只存非零元素，**且必须按偏移升序**（§4.3 的契约）。`:zero` 与
//      `:data` **互斥**：`allZero == true` 时 `nonzero` 必为空。
// ============================================================================
#ifndef SYSY_FRONTEND_INITPLAN_H
#define SYSY_FRONTEND_INITPLAN_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "frontend/ConstEval.h"
#include "frontend/Type.h"
#include "support/SourceLoc.h"

namespace sysy {

struct Expr;

// ── 一条写动作的种类（prompt §四.1 的四种）────────────────────────────────
enum class InitActionKind {
  Zero,         // 从 offset 起，bytes 个字节清零
  StoreConst,   // 在 offset 处写入一个编译期常量
  StoreExpr,    // 在 offset 处写入一个运行期表达式的值（局部才有）
  MemcpyConst,  // 从 offset 起，写入一串编译期常量（行主序、连续）
};

inline const char* initActionName(InitActionKind k) {
  switch (k) {
    case InitActionKind::Zero:        return "Zero";
    case InitActionKind::StoreConst:  return "StoreConst";
    case InitActionKind::StoreExpr:   return "StoreExpr";
    case InitActionKind::MemcpyConst: return "MemcpyConst";
  }
  return "?";
}

// ── 一条写动作 ─────────────────────────────────────────────────────────────
//   Zero        : [offset, offset+bytes) 清零（bytes 是**字节数**）
//   StoreConst  : 把 `value` 写进 offset（int→4 字节、float→4 字节）
//   StoreExpr   : 把运行期表达式 `expr` 的值写进 offset（**只有局部**才有）
//   MemcpyConst : 从 offset 起连续写 `values`（行主序、无空洞）
//   ⚠️ `expr` **不持有**（只是指向 AST 的借用指针）：AST 的生命期覆盖整个
//      emit 过程（main 持有 CompUnit），而计划只是它的一个视图。
struct InitAction {
  InitActionKind kind = InitActionKind::Zero;
  uint64_t offset = 0;                     // 相对对象起始的字节偏移
  uint64_t bytes = 0;                      // Zero 用：清零的字节数
  ConstValue value;                        // StoreConst 用
  std::vector<ConstValue> values;          // MemcpyConst 用
  const Expr* expr = nullptr;              // StoreExpr 用（不持有）
  SourceLoc loc;                           // 这条动作对应的源码位置
};

struct InitPlan {
  // ── 全局对象 ────────────────────────────────────────────────────────────
  //   ★ `allZero` 是**强制**的省法（prompt §4.2）：语料里 6 个文件各有
  //     2×864 MB 的全局数组。列出全 0..N-1 会让转储上到 GB 级。
  struct GlobalData {
    std::string name;
    Type type;                       // 按值存：Type 是 3 个标量的 POD
    bool allZero = true;
    std::vector<std::pair<uint64_t, ConstValue>> nonzero;   // 偏移(字节)+值，升序
    SourceLoc loc;
  };

  // ── 局部对象 ────────────────────────────────────────────────────────────
  struct LocalInit {
    std::string name;                // `函数名/变量名`（同名遮蔽时区分用，§4.3）
    Type type;
    std::vector<InitAction> actions; // **按执行顺序**
    SourceLoc loc;
  };

  // ★ 顺序 = 源文件中的声明顺序（§4.3 的契约）。
  std::vector<GlobalData> globals;
  std::vector<LocalInit> locals;
};

}  // namespace sysy
#endif  // SYSY_FRONTEND_INITPLAN_H
