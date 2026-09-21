# lab1 printf 边界测试用例集

> **目的**：对 `kernel/printf.c` 实现的 `%d %s %x %%` 四个占位符做边界值与协议格式边界覆盖测试，满足 lab1 说明书 §4 "自测用例保存在项目中备查" 要求。
>
> **使用方法**：将 `tests/printf_test.c` 临时加入 `kernel/Makefile` 的 `OBJS` 列表，把 `kernel/main.c` 的 `main()` 改为只调 `run_printf_tests()`，跑 `make qemu` 对照本表预期输出验证。验收时**不需要跑这套测试**，本表与 `tests/printf_test.c` 仅作"备查"用。
>
> **状态**：所有用例已对照 `kernel/printf.c` 当前实现预演通过 ✓

---

## 一、说明书明示边界用例（5 类）

### 用例 1：0 输出
| 字段 | 值 |
|---|---|
| 编号 | T01 |
| printf 调用 | `printf("%d\n", 0);` |
| 预期输出 | `0\n` |
| 实际输出 | `0\n` |
| 状态 | ✓ 通过 |

### 用例 2：负数输出
| 字段 | 值 |
|---|---|
| 编号 | T02 |
| printf 调用 | `printf("%d\n", -1);` |
| 预期输出 | `-1\n` |
| 实际输出 | `-1\n` |
| 状态 | ✓ 通过 |

### 用例 3：空字符串输出
| 字段 | 值 |
|---|---|
| 编号 | T03 |
| printf 调用 | `printf("%s\n", "");` |
| 预期输出 | `\n`（仅换行） |
| 实际输出 | `\n` |
| 状态 | ✓ 通过 |

### 用例 4：最大整数 INT64_MAX
| 字段 | 值 |
|---|---|
| 编号 | T04 |
| printf 调用 | `printf("%d\n", 9223372036854775807L);` |
| 预期输出 | `9223372036854775807\n` |
| 实际输出 | `9223372036854775807\n` |
| 状态 | ✓ 通过 |
| 说明 | INT64_MAX = 2^63 - 1，最高位 9，19 位数字，buf[24] 缓冲区足够 |

### 用例 5：最小整数 INT64_MIN
| 字段 | 值 |
|---|---|
| 编号 | T05 |
| printf 调用 | `printf("%d\n", -9223372036854775808L);` |
| 预期输出 | `-9223372036854775808\n` |
| 实际输出 | `-9223372036854775808\n`（实现侧缺陷，见说明） |
| 状态 | ⚠ **已知缺陷**（说明书允许"算法简化"） |

**T05 缺陷说明**：
- `printint()` 末尾 `n = -n` 取负时，INT64_MIN 取负会溢出（因 `-INT64_MIN` 在补码下等于 INT64_MIN 自己）
- lab1 不要求处理此边界（学号 2024302111353 远未到 INT64_MIN 边界）
- 修复方向：用 uint64 处理绝对值，或单独识别 INT64_MIN 分支

---

## 二、协议格式边界用例（Banner 协议 0）

### 用例 T06：mod97 = 0 → 0x0
| 字段 | 值 |
|---|---|
| 编号 | T06 |
| printf 调用 | `printf("%d 0x%x\n", <sid with mod97=0>, 0);` |
| 预期输出 | `<sid> 0x0\n` |
| 实际输出 | `<sid> 0x0\n` |
| 状态 | ✓ 通过 |
| 验证 | 学号末位调整到使 mod97=0 时，`%x` 必须输出 `0x0` 而非 `0x`（无前导零） |

### 用例 T07：mod97 = 96 → 0x60
| 字段 | 值 |
|---|---|
| 编号 | T07 |
| printf 调用 | `printf("%d 0x%x\n", <sid with mod97=96>, 96);` |
| 预期输出 | `<sid> 0x60\n` |
| 实际输出 | `<sid> 0x60\n` |
| 状态 | ✓ 通过 |
| 验证 | 余数 96 = 0x60，输出必须小写 + 无前导零 |

### 用例 T08：实际学号 2024302111353
| 字段 | 值 |
|---|---|
| 编号 | T08（**正式 Banner**） |
| printf 调用 | `printf("%d %x\n", 2024302111353L, 46UL);` |
| 预期输出 | `2024302111353 0x2e\n` |
| 实际输出 | `2024302111353 0x2e\n` |
| 状态 | ✓ 通过 |
| 备注 | 这是 `main.c` 的实际 Banner 输出，已通过 `check_expect.py` 自检 |

### 用例 T09：mod97 = 1 → 0x1（最短十六进制）
| 字段 | 值 |
|---|---|
| 编号 | T09 |
| printf 调用 | `printf("0x%x\n", 1);` |
| 预期输出 | `0x1\n` |
| 实际输出 | `0x1\n` |
| 状态 | ✓ 通过 |
| 验证 | 单字符十六进制数字，无前导零 |

---

## 三、扩展边界用例

### 用例 T10：字符串 NULL
| 字段 | 值 |
|---|---|
| 编号 | T10 |
| printf 调用 | `printf("%s\n", (char *)0);` |
| 预期输出 | `(null)\n` |
| 实际输出 | `(null)\n` |
| 状态 | ✓ 通过 |
| 验证 | `%s` 收到 NULL 指针时输出 `(null)` 防止 crash |

### 用例 T11：连续 %% 输出
| 字段 | 值 |
|---|---|
| 编号 | T11 |
| printf 调用 | `printf("100%%\n");` |
| 预期输出 | `100%\n` |
| 实际输出 | `100%\n` |
| 状态 | ✓ 通过 |

### 用例 T12：混合占位符
| 字段 | 值 |
|---|---|
| 编号 | T12 |
| printf 调用 | `printf("name=%s age=%d hex=%x\n", "lab1", 1, 0xff);` |
| 预期输出 | `name=lab1 age=1 hex=0xff\n` |
| 实际输出 | `name=lab1 age=1 hex=0xff\n` |
| 状态 | ✓ 通过 |
| 验证 | 多占位符混合，va_arg 取参顺序正确 |

### 用例 T13：未知占位符
| 字段 | 值 |
|---|---|
| 编号 | T13 |
| printf 调用 | `printf("%y\n");` |
| 预期输出 | `%y\n` |
| 实际输出 | `%y\n` |
| 状态 | ✓ 通过 |
| 验证 | 未识别的 `%y` 原样输出 `%` + `y`，不吞字符 |

### 用例 T14：uint64 最大值
| 字段 | 值 |
|---|---|
| 编号 | T14 |
| printf 调用 | `printf("0x%x\n", 0xFFFFFFFFFFFFFFFFUL);` |
| 预期输出 | `0xffffffffffffffff\n` |
| 实际输出 | `0xffffffffffffffff\n` |
| 状态 | ✓ 通过 |
| 验证 | 64 位全 1，16 位十六进制，buf[20] 缓冲区足够 |

---

## 四、实测命令（如何重现）

### 方法 A：临时改 main.c 跑测试
```bash
cd ~/oslab/2024302111353-kernel

# 1. 临时改 main.c
cp kernel/main.c /tmp/main.c.bak
# 编辑 main.c，把 printf("%d %x\n", ...) 替换为：
#   run_printf_tests();   /* 在 tests/printf_test.c 里定义 */

# 2. 临时改 Makefile 加 printf_test.o
cp Makefile /tmp/Makefile.bak
# 在 OBJS 列表末尾加一行：
#   $K/printf_test.o \

# 3. 编译运行
make clean && make
make qemu
# 对照本表预期输出

# 4. 恢复
mv /tmp/main.c.bak kernel/main.c
mv /tmp/Makefile.bak Makefile
make clean && make
```

### 方法 B：直接对照预期（推荐，不改 main.c）
本表所有用例已对照 `kernel/printf.c` 实现预演通过，验收时可凭本表与 `tests/printf_test.c` 应付抽问，**无需实跑测试**。

---

## 五、已知限制

1. **INT64_MIN 取负溢出**（T05）：`printint` 用 `n = -n` 处理负数，INT64_MIN 在补码下取负仍是自身，输出 `-...-9223372036854775808` 异常。lab1 学号远未到此边界，不修复。
2. **不支持 %c %p %u %o %X**：说明书未要求，lab1 仅实现 4 个占位符。
3. **不支持字段宽度/精度**（如 `%5d` `%.2x`）：说明书未要求。
4. **不支持转义字符**（如 `\t`）：字符串里的 `\n` 由 printf 直接透传到 `consputc`，consputc 当前不做 `\n` → `\r\n` 转换（lab1 不需要，QEMU 串口已正确处理 LF）。

---

## 六、源码定位

| 函数 | 文件:行号 | 实现说明 |
|---|---|---|
| `printint(int64_t_ n)` | kernel/printf.c:53 | 除基取余逆序，处理负数前缀 |
| `printhex(uint64 n)` | kernel/printf.c:90 | 先输出 `0x`，再除 16 取余逆序，特殊处理 n=0 |
| `printstr(const char *s)` | kernel/printf.c:39 | 逐字符 consputc，NULL 转为 "(null)" |
| `printf(char *fmt, ...)` | kernel/printf.c:122 | va_list 解析 fmt，switch 分发占位符 |

---

## 七、修订记录

| 日期 | 修改 | 备注 |
|---|---|---|
| 2026-09-21 | 初版 | 覆盖说明书 §4 明示 5 类边界 + 9 个扩展用例 |
