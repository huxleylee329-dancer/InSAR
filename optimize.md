# InSAR 项目代码优化清单

> 审查日期：2026-06-07
> 按 Visual Studio 工程为单位组织，每个工程内按问题等级排序。
> 修复状态：`[ ]` 未修复 → `[x]` 已修复

---

## 目录

- [ComplexMat](#complexmat)
- [Utils](#utils)
- [Registration](#registration)
- [Deflat](#deflat)
- [Filter](#filter)
- [Unwrap](#unwrap)
- [FormatConversion](#formatconversion)
- [SBAS](#sbas)
- [Dem](#dem)
- [Evaluation](#evaluation)
- [simulation](#simulation)
- [include（公共头文件）](#include公共头文件)
- [跨模块共性问题](#跨模块共性问题)

---

## ComplexMat

### P0 — 必须修复

- [x] **`operator+` 维度检查 bug** — `ComplexMat.cpp:324`
  - `b.GetRows() != b.GetRows()` 永远为 false，应为 `this->GetRows() != b.GetRows()`
  - 行数不匹配时不触发错误检查，可能导致崩溃

### P1 — 重要

- [x] **`operator=` 返回值应为引用** — `ComplexMat.h:50`, `ComplexMat.cpp:342-347`
  - 当前签名为 `ComplexMat operator=(const ComplexMat&)`，应改为 `ComplexMat& operator=(const ComplexMat&)`
  - 禁止链式赋值 `a = b = c`，且产生不必要的临时对象
- [x] **头文件中 `using namespace std` 污染全局命名空间** — `ComplexMat.h:11`
  - 所有包含此头文件的翻译单元都被迫引入 `std` 命名空间
  - 应改为显式使用 `std::complex` 等
- [x] **头文件中 `using cv::Mat` 在全局作用域** — `ComplexMat.h:10`
  - 同上，应改为在类内部或函数参数中使用完整限定名 `cv::Mat`
- [x] **`countNonzero()` 硬编码 `double` 类型** — `ComplexMat.cpp:480-504`
  - 矩阵类型不是 CV_64F 时会读取错误数据，应根据 `type()` 分支处理
- [x] **`sum()` 硬编码 `double` 类型** — `ComplexMat.cpp:349-400`
  - 同上

### P2 — 建议

- [ ] **`Mul` 与 `mul` 命名极易混淆** — `ComplexMat.h:34,36`
  - `mul` 是矩阵乘法，`Mul` 是逐元素共轭乘法
  - 建议重命名为 `matMul` 和 `elementMul`
  - 跳过：改名影响全局调用方，待统一处理
- [x] **`GetPhase()` 未标记为 `const`** — `ComplexMat.h:27`, `ComplexMat.cpp:211`
  - 不修改对象状态，应与 `GetRe()`/`GetIm()`/`GetMod()` 风格一致
- [x] **`GetPhase()` 四个类型分支高度重复** — `ComplexMat.cpp:220-263`
  - CV_64F/CV_32F/CV_32S/CV_16S 四个分支结构完全相同
  - 建议使用 C++ 模板函数进行去重，避免转换成 CV_64F 带来的临时矩阵内存翻倍开销
- [x] **`operator*` 运算符优先级隐患** — `ComplexMat.cpp:90-91`
  - `&&` 和 `||` 混用缺少括号，建议显式加括号
- [x] **`GetPhase()` 对不支持的类型静默返回未初始化矩阵** — `ComplexMat.cpp:211-266`
  - 应添加 `else` 分支返回错误
- [x] **`conj()` 中不必要的深拷贝** — `ComplexMat.cpp:437-461`
  - 先 `copyTo` 再取负再 `SetRe`/`SetIm`，可减少一次完整矩阵拷贝

### P3 — 优化

- [x] **私有成员 `mod`/`Phase` 从未使用** — `ComplexMat.h:73-74`
  - 应移除以减少内存占用
- [ ] **`isempty()` 命名不符合驼峰规范** — `ComplexMat.h:66`
  - 应改为 `isEmpty()`
  - 跳过：改名影响 4 个文件 12 处调用，待统一处理
- [x] **头文件注释风格不统一** — `ComplexMat.h`
  - 部分 `/* */`，部分 Doxygen 风格，建议统一

---

## Utils

### P0 — 必须修复

- [x] **`createVandermondeMatrix` 注释返回值描述反了** — `Utils.h:327`
  - 注释写"成功返回-1，否则返回0"，实际是成功返回 0、失败返回 -1
  - 会误导使用者写出错误的错误处理逻辑
  - 修复：改为"成功返回0，否则返回-1"；顺带为 Utils.h 和 FormatConversion.h 中 `ployFit` 补充 `@return` 注释

### P1 — 重要

- [ ] **文件过大：Utils.cpp 13792 行** — `Utils/Utils.cpp`
  - 建议按功能拆分为：`Utils_DIMACS.cpp`、`Utils_Coherence.cpp`、`Utils_Geo.cpp`、`Utils_Delaunay.cpp`、`Utils_Multilook.cpp`
- [ ] **头文件过大：Utils.h 1937 行** — `include/Utils.h`
  - 建议按功能领域拆分
- [x] **`parallel_flag_change` 按值传递 volatile，修改无效** — `Utils.cpp:76-87`
  - 按值传递的 `volatile bool` 只修改栈上副本，对调用者无效
  - 该函数实际未被调用，属于死代码，应删除
  - 修复：删除该函数定义
- [x] **`geo_transformation` 硬编码绝对路径** — `Utils.cpp:12955`
  - `D:\softwarepackages\release-1928-x64-gdal-...` 在非开发机上会失败
  - 应改为配置参数或环境变量
  - 修复：提取 `setupProjSearchPaths()` 函数，优先读 `PROJ_DATA`（PROJ 9+），回退 `PROJ_LIB`（PROJ 7/8），未设置时打印警告；3 处硬编码路径全部替换
- [x] **`OGRCreateCoordinateTransformation` 在循环内重复创建** — `Utils.cpp:12956-12962`
  - 极其昂贵的操作，应提到循环外
  - 修复：将 `setupProjSearchPaths()`、`OGRSpatialReference`、`OGRCreateCoordinateTransformation` 全部移到 OMP 循环外；修复 `coordTrans` 内存泄漏（添加 `delete`）
- [x] **`write_DIMACS` 四个重载版本大量重复** — `Utils.cpp:252-1266`
  - 统计残差点、计算正负平衡、写 DIMACS 头部等逻辑重复
  - 应抽取公共内部辅助函数
  - 跳过：重载 1（指针版）和重载 2（vector 版）表面相似但存在关键差异，不宜直接合并：
    - 无残差点判断：重载 1 用 `||`（任一为零即退出），重载 2 用 `&&`（都为零才退出）
    - 弧流量上界：重载 1 用 `upper_bound = 5`，重载 2 用 `upper_bound = 1`
  - 建议：检查这两处差异是有意设计还是 bug，确认后再决定是否重构
- [x] **`read_DIMACS` 三个重载版本重复** — `Utils.cpp:602-2600+`
  - 注释解析、目标值解析、流结果解析代码重复
  - 跳过：三个重载仅文件头解析（约 55 行）相同，流结果解析因数据模型不同（网格 vs 三角网）差异较大，不宜强行合并
  - 建议：重载 2（指针版）缺少接地边处理（重载 3 有），检查是否为遗漏
- [x] **`real_coherence` 两个版本大量重复** — `Utils.cpp:1764-1880`
  - 默认窗口版和自定义窗口版的相干计算循环体几乎完全相同
  - 跳过：重复代码仅约 30 行，重载 1 是重载 2 在窗口 3×3 时的特例，收益有限
- [x] **`complex_coherence` 两个版本大量重复** — `Utils.cpp:1882-2052`
  - 同上，且 CV_64F/CV_32F 分支又是一次重复
  - 跳过：与 `real_coherence` 同类问题，重载 1 是重载 2 在窗口 3×3 时的特例；重载 2 内部 CV_64F/CV_32F 分支重复约 30 行；总重复约 60 行，收益有限
- [x] **地理编码中双线性插值逻辑大量重复** — `Utils.cpp:12650-13100+`
  - `geo2sar_DLR` 两个版本中 UTM_x/UTM_y 插值代码几乎逐行重复
  - 应抽取为 `bilinear_interpolate()` 辅助函数
  - 跳过：两个 `geo_transformation` 重载中 54 行完全相同的双线性插值代码，但提取辅助函数需传入多个矩阵引用，接口收益有限
- [ ] **数据结构体定义与 Utils 类耦合** — `Utils.h:126-309`
  - `triangle`、`tri_edge`、`tri_node`、`node_index`、`edge_index` 应提取到 `DelaunayTypes.h`

### P2 — 建议

- [x] **`residue` 指针版与 vector 版核心逻辑重复** — `Utils.cpp:1530-1690`
	  - 跳过：两版本存在实际差异 — y 分量符号相反、方向判断相反（`>` vs `<`）、距离阈值不同（硬编码 50.0 vs 参数，最小 2.0），不宜直接合并
- [x] **`bin2cvmat` 中不必要的 `malloc`+`memcpy`** — `Utils.cpp:2703-2712`
  - 可直接 `fread` 到 `Mat::data` 指针
  - 修复：移除 `malloc`/`memcpy`/`free`，`fread` 直接读入 `Mat::data`
- [ ] **赋值运算符返回值而非引用** — `Utils.h` 多处
  - `tri_node`、`triangle`、`tri_edge`、`node_index` 的 `operator=` 应返回引用
- [x] **`PI` 精度不一致** — `Utils.cpp:1523,3025,3080,3117`
  - 部分手写 `3.1415926535`（11 位），部分 `3.14159265358979323846`（21 位）
  - 应统一使用 `Package.h` 中的 `PI` 宏
  - 修复：`Package.h` 中 `PI` 精度从 18 位提升到 20 位；Utils.cpp 中 4 处局部 `pi` 变量全部替换为 `PI` 宏
- [x] **`volatile int count` 在 OpenMP 并行区域中使用** — `Utils.cpp:12658,12950`
  - `volatile` 不保证原子性，应使用 `std::atomic<int>`
  - 修复：添加 `#include <atomic>`，两处 `volatile int count` 替换为 `std::atomic<int> count(0)`
- [ ] **`gen_mask` 系列函数中每次循环创建临时 Mat** — `Utils.cpp:1409,1721` 等
  - 在 OMP 并行的双层循环内每次迭代调用 `cv::mean()`，有性能开销
  - 跳过：`cv::mean()` 本身不创建大 Mat（ROI 仅创建轻量头部），真正瓶颈是每个像素 O(wnd_size²) 的均值计算；建议改用积分图（`cv::integral`）或 `cv::boxFilter` 降至 O(1)，但属算法层面重构，改动较大

### P3 — 优化

- [ ] **函数名拼写错误** — `Utils.h:1442`、`Utils.h:338`
  - `computeImageGeoBoundry` → `computeImageGeoBoundary`
  - `ployFit` → `polyFit`
  - `defficiency` → `deficiency`（`Utils.cpp:135`）
- [ ] **命名风格不一致** — `Utils.h`
  - 类名 `ComplexMat`(驼峰) vs `tri_node`(下划线) vs `Utils`(首字母大写)
  - 建议统一为驼峰命名
- [ ] **大量被注释掉的代码未清理** — `Utils.cpp:4830-4880+` 等
	  - 跳过：共约 1270 行注释代码，其中 `stack_coregistration` 三个废弃版本约 827 行、`MB_phase_estimation` 约 251 行、HermitianEVD 相位估计约 68 行；部分可能还会启用，暂不清理
- [x] **`ofstream fout` 声明但从未使用** — `Utils.cpp:837,1118`
	  - 修复：两处声明注释掉，标注"未使用"
- [x] **`GET_NEXT_LINE` 宏缩进误导** — `Utils.cpp:27-39`
  - `else` 分支缩进对齐错误，建议改为内联函数
  - 修复：统一 `else` 缩进为 4 空格，与 `if` 对齐
- [x] **`using namespace std` 在 Utils.cpp 中** — `Utils.cpp:25`
  - 虽在 .cpp 中影响较小，但仍不推荐
  - 跳过：实际检查发现 Utils.cpp 中不存在 `using namespace std;`，仅有 `using namespace cv;`，原始记录有误

---

## Registration

### P0 — 必须修复

（无）

### P1 — 重要

- [ ] **`Orbit_Polyfit` 仅属于 Deflat 模块**（原误记在 `Registration` 模块，已修正，见 Deflat P0 建议）
- [ ] **`interp_cubic` 两个重载中边界扩展代码完全重复** — `Registration.cpp:312-583`
  - 约 120 行 copyTo 操作完全一致
  - 应抽取为 `padBorderComplex()` 辅助函数
- [ ] **双线性插值逻辑重复** — `Registration.cpp:1070-1153` vs `1713-1798`
  - `coregistration_subpixel` 和 `performBilinearResampling` 中三分支双线性插值重复约 60 行
  - 建议使用模板或统一的类型无关插值函数
- [ ] **OMP 并行循环内逐行创建 Mat 对象** — `Registration.cpp:1070-1076,1714-1718`
  - 每行创建 `Mat tmp` 和 `Mat result`（实际是逐行创建，并非逐像素，但大图时仍会产生数万次频繁分配与锁竞争）
  - 应移到循环外预分配
- [ ] **`return_check`/`parallel_check`/`parallel_flag_change` 与其他模块重复** — `Registration.cpp:19-56`
  - 应提取到公共头文件

### P2 — 建议

- [ ] **`WeightCalculation` 中自赋值** — `Registration.cpp:590`
  - `offset = offset;` 无效，整个 if-else 块等价于 `offset = fabs(offset);`
- [ ] **函数参数过多** — `Registration.h:127-146`
  - `getDEMRgAzPos` 共 18 个参数，建议封装为结构体
- [ ] **缺少 `const` 修饰** — `Registration.h` 多处
  - `real_coherent` 的 Master/Slave 仅被读取，应为 `const ComplexMat&`
  - `all_subpixel_move` 的输入参数应为 `const Mat&`
- [ ] **`Mat` 参数按值传递** — `Registration.h` 部分重载
  - `auxi`、`gcps`、`orbit_main`、`orbit_slave` 应改为 `const Mat&`

### P3 — 优化

- [ ] **头文件 include guard 冗余** — `Registration.h:1-2`
  - 同时使用 `#pragma once` 和 `#ifndef`，建议统一
- [ ] **include 路径缺少空格** — `Registration.h:4-6`
  - `#include"..\include\Package.h"` 应为 `#include "..\include\Package.h"`
- [ ] **错误信息拼写错误** — `Registration.cpp:1044,1049,1212`
  - `"matrix defficiency"` → `"matrix deficiency"`
  - `"cant'"` → `"can't"`
- [ ] **函数名与错误信息不匹配** — `Registration.cpp:775,837,991`
  - `coregistration_subpixel` 中错误消息写的是 `"coregistration_pixel()"`
- [ ] **注释掉的调试代码未清理** — `Registration.cpp:661,756-757,876-877,896-900,914-954`
- [ ] **魔数散布** — `Registration.cpp:859`（0.65 相干性阈值）、`784`（10000 裁剪尺寸）

---

## Deflat

### P0 — 必须修复

- [ ] **`Orbit_Polyfit` 奇异矩阵检测永远为假** — `Deflat.cpp:199`
  - `if (fabs(ret) < 0.0)` 永远为 false
  - 应改为 `if (fabs(ret) < 1e-12)`

### P1 — 重要

- [ ] **文件过大：2887 行，职责过多** — `Deflat/Deflat.cpp`
  - 建议拆分：SRTM 相关功能（~760 行）移至 `SRTMManager` 类
  - DEM 映射功能（~670 行）可考虑独立
- [ ] **零多普勒时间搜索代码重复 5 处** — `Deflat.cpp:951-1047,1224-1322,1625-1703,1790-1868`
  - 每处约 60-80 行，与 Registration/Utils 中的版本完全相同
  - 应调用共享的 `findZeroDopplerTime()` 函数
- [ ] **`getSRTMFileName` 格式化逻辑重复 8+ 次** — `Deflat.cpp:2122-2331`
  - 可用 `sprintf(buf, "srtm_%02d_%02d.zip", col, row)` 一行替代所有 if-else
- [ ] **`getSRTMDEM` 中 tif 路径构建重复 15 次** — `Deflat.cpp:2438-2880`
  - 应抽取为辅助函数
- [ ] **`get_satellite_aztime_NEWTON` 不检测迭代发散** — `Deflat.cpp:152-191`
  - 始终返回 0（成功），即使迭代 15 次未收敛也不会报错
  - 应添加发散检测
- [ ] **`return_check`/`parallel_check`/`parallel_flag_change` 与其他模块重复** — `Deflat.cpp:23-61`
  - 应提取到公共头文件

### P2 — 建议

- [ ] **函数参数过多** — `Deflat.h` 多处
  - `topo_removal` 17 个参数、`topography_simulation` 18 个参数、`demMapping` 18-20 个参数
  - 建议封装为结构体
- [ ] **`mode` 参数使用魔法数字** — `Deflat.cpp` 多处
  - 1/2/4 分别对应"单发单收"/"单发双收"/"双频乒乓"，应使用枚举
- [ ] **变量命名混乱** — `Deflat.cpp:490,718`
  - `int xxxx` 作为轨道索引，应命名为 `orbitIdx`
  - `a0,a1,a2,a3,a4,a5,offset_inc,scale_inc` 应定义结构体
- [ ] **`demMapping` 中 DEM 插值搜索填充算法重复** — `Deflat.cpp:1050-1149` vs `1323-1561`
  - 两个重载中约 100 行搜索填充逻辑完全相同

### P3 — 优化

- [ ] **魔数散布** — `Deflat.cpp:273,274,275`
  - `3.1415926535` 应使用 `PI` 宏
  - `300000000.0` 应使用 `VEL_C` 常量
  - `0.0000454` 牛顿迭代收敛阈值应命名常量化
- [ ] **错误信息拼写错误** — `Deflat.cpp:2348`
  - `"download failded"` → `"download failed"`
- [ ] **函数名与错误信息不匹配** — `Deflat.cpp:1754`
  - `slantrange_compute` 中错误消息写的是 `"SLC_deramp()"`
- [ ] **大量注释掉的代码未清理** — `Deflat.cpp:876-877,1591-1607,2052-2057`
- [ ] **头文件 include guard 冗余** — `Deflat.h:1-2`

---

## Filter

### P0 — 必须修复

- [ ] **`Dst.zeros()` 静态方法误用为实例方法** — `Filter.cpp:823`
  - `Mat::zeros` 是静态工厂方法，不会修改已有对象 `Dst`
  - 应使用 `Dst.setTo(0)`

### P1 — 重要

- [ ] **`Goldstein_filter` 与 `Goldstein_filter_parallel` 约 200 行重复代码** — `Filter.cpp:549-812`
  - 两个函数约 80% 代码完全一致
  - 应重构为公共辅助函数，两个接口仅在并行策略上有差异
- [ ] **`new TCHAR[512]` 裸分配存在泄漏风险** — `Filter.cpp:475`
  - 建议使用 `std::wstring` 或 `std::vector<wchar_t>`
- [ ] **使用已废弃的 `USES_CONVERSION`/`A2W` 宏** — `Filter.cpp:474`
  - 使用线程局部栈空间进行编码转换，大循环中会导致栈溢出
  - 建议使用 `MultiByteToWideChar`
- [ ] **`return_check`/`parallel_check`/`parallel_flag_change` 与其他模块重复** — `Filter.cpp:16-53`
  - 应提取到公共头文件
- [ ] **`parallel_flag_change` 按值传递 volatile，修改无效** — `Filter.cpp:42-53`
  - 死代码，应删除

### P2 — 建议

- [ ] **OMP 并行循环内 `fprintf(stdout, ...)` 输出交错** — `Filter.cpp:427`
  - 多线程并发输出会导致交错混乱
  - 应使用 `#pragma omp critical` 或仅在主线程打印
- [ ] **`slope_adaptive_filter` 并行循环内大量 Mat 对象创建** — `Filter.cpp:321-428`
  - 每个像素创建多个 Mat 对象，百万级像素时性能严重下降
  - 应预分配工作缓冲区并在循环内复用
- [ ] **`PI` 硬编码精度不足** — `Filter.cpp:258`
  - `double pi = 3.1415926535;` 应使用 `Package.h` 中的 `PI` 宏

### P3 — 优化

- [ ] **头文件 include guard 冗余** — `Filter.h:1-2`
- [ ] **注释风格不一致** — `Filter.h:15-86`
  - 部分 `/*...*/`，部分 `/** @brief`，部分 `//`，建议统一为 Doxygen
- [ ] **`GaussianFilter` 参数缺少 const** — `Filter.h:82`
  - `window` 参数不被修改，应为 `const Mat&`
- [ ] **大量注释掉的代码未清理** — `Filter.cpp:121-131,319-320,350,363-364,429`
- [ ] **`error_head` 使用裸 `char[]`** — `Filter.h:84-86`
  - 建议使用 `std::string`

---

## Unwrap

### P0 — 必须修复

- [ ] **`slave.convertTo` bug** — `Unwrap.cpp:2590`
  - `if (slave.type() != CV_64F) master.convertTo(slave, CV_64F);`
  - 检查的是 slave 类型，转换的却是 master，导致 slave 数据被 master 覆盖
  - 应改为 `slave.convertTo(slave, CV_64F);`
- [ ] **`CHECK_RETURN` 宏有严重缺陷** — `Unwrap.cpp:10-16`
  - 宏中 `if` 只控制 `strncpy` 一行，`fprintf` 和 `return -1` 无条件执行
  - 调用时函数会无条件返回 -1
  - 应立即删除（当前未被使用）
- [ ] **`quality.at<int>` 类型不匹配** — `Unwrap.cpp:1067,2927`
  - `quality` 为 CV_64F 类型但用 `at<int>` 访问，会读取错误值
  - 应改为 `quality.at<double>(i, j)`

### P1 — 重要

- [ ] **文件过大：3239 行，13 个公开方法** — `Unwrap/Unwrap.cpp`
  - 建议按算法类型拆分：`MCF_Unwrap.cpp`、`QualityGuided_Unwrap.cpp`、`Snaphu_Unwrap.cpp`、`SPD_Unwrap.cpp`
- [ ] **约 670 行 `#if 0` 死代码** — `Unwrap.cpp:386-1057`
  - 策略 1-5 全部被禁用但保留，应完全删除（Git 保留历史）
- [ ] **外部进程创建代码重复 6 次** — `Unwrap.cpp:146-198,265-318,1757-1810,2692-2744,2829-2885`
  - 每处约 40-50 行 `CreateProcess`+Job Object+`WaitForSingleObject`+句柄关闭
  - 应提取为 `runExternalProcess()` 辅助函数
- [ ] **MCF 系列函数 BFS 解缠核心逻辑重复 4 次** — `Unwrap.cpp:1255,1417,1581,1813`
  - BFS 遍历、邻居查找、相位梯度计算、解缠赋值代码几乎完全相同
  - 应提取核心 BFS 解缠循环为私有辅助方法
- [ ] **硬编码调试路径** — `Unwrap.cpp:264,324-325,2297-2299`
  - `E:\zgb1\functions\mask.bin`、`E:\working_dir\projects\software\InSAR\bin\...`
  - 应立即删除
- [ ] **`szCommandLine` 缓冲区溢出风险** — `Unwrap.cpp:146,1757,2692`
  - `new TCHAR[256]`，路径较长时 `wcscpy`/`wcscat` 会溢出
  - 建议使用 `std::wstring` 动态构建命令行
- [ ] **`return_check`/`parallel_check`/`parallel_flag_change` 与其他模块重复** — `Unwrap.cpp:27-64`

### P2 — 建议

- [ ] **`MCF` 三个重载签名差异不明显** — `Unwrap.h:68-83`
  - 后两个重载仅在 `tri_edge*` vs `vector<tri_edge>&` 上有微妙区别
  - 建议使用不同函数名：`MCF_regular`、`MCF_irregular`
- [ ] **`tri_edge*` 裸指针配合 `int num_edges`** — `Unwrap.h:73`
  - 应改为 `vector<tri_edge>&`（第三个重载已迁移）
- [ ] **`MCF_second` 中 `pass` 参数无实际作用** — `Unwrap.cpp:1599-1603`
  - 无论 `pass` 为何值，`tt` 都是 100000.0
- [ ] **变量名遮蔽 `std::min`/`std::max`** — `Unwrap.cpp:1289-1290,1455-1456`
  - 使用 `min`/`max` 作为变量名
- [ ] **未解缠像素填充值可能不合理** — `Unwrap.cpp:1402-1412`
  - `min - 0.1*(max - min)` 可能在后续处理中引入伪影，建议使用 NaN

### P3 — 优化

- [ ] **函数名拼写错误** — `Unwrap.h`
  - `quailtyGuidedFloodfill` → `qualityGuidedFloodfill`
- [ ] **四方向邻居处理代码重复** — `Unwrap.cpp:1079-1136`
  - 可使用方向偏移数组 `dx[]/dy[]` 简化为循环
- [ ] **`globalparam.h` 使用相对路径不一致** — `Unwrap.h:7`
  - 其他头文件使用 `..\include\` 路径
- [ ] **注释与代码不一致** — `Unwrap.cpp:1599`
  - 注释说 `pass` 用于"绕过枝切线"，但实际行为相同

---

## FormatConversion

### P0 — 必须修复

- [x] **HDF5 `memspace_id` 未关闭（资源泄漏）** — `FormatConversion.cpp:539`
  - `read_subarray_from_h5` 中创建后从未关闭
  - `write_subarray_to_h5` 中同样存在（第 692 行）
  - 修复：在两个函数的资源清理代码中添加 `H5Sclose(memspace_id);`
- [x] **多项式拟合代码重复 4 处（~800 行）** — `FormatConversion.cpp:1040-1449,2225-2635,9029+`
  - 手动构建范德蒙矩阵逻辑完全相同
  - 头文件已声明 `createVandermondeMatrix` 和 `ployFit` 但实现中未使用
  - 修复：实现 `createVandermondeMatrix()` 和 `polyFit()` 两个公共函数，替换前 3 处重复代码（TerraSAR-X + 2 个 Sentinel-1），第 4 处（ALOS）保持独立（使用 1 阶线性拟合）
  - 效果：减少约 325 行代码（从 13921 行减少到 13596 行）

### P1 — 重要

- [ ] **文件过大：13921 行，11 个类** — `FormatConversion/FormatConversion.cpp`
  - 必须拆分：`FormatConversion_h5.cpp`、`FormatConversion_TSX.cpp`、`FormatConversion_Sentinel.cpp`、`FormatConversion_ALOS.cpp`、`XMLFile.cpp`、`Sentinel1Reader.cpp` 等
- [ ] **头文件过大：2213 行，11 个类** — `include/FormatConversion.h`
  - 建议各类拆分到独立头文件，在 FormatConversion.h 中用 `#include` 聚合
- [ ] **传感器读取器类字段重复** — `FormatConversion.cpp:1364-1592`
  - `CSK_reader`、`HTHT_reader`、`LUTAN_reader`、`Spacety_reader` 成员变量几乎完全相同
  - 应提取公共基类 `SARDataReader`
- [ ] **H5 类型映射 if-else 链重复** — `FormatConversion.cpp:179-198,249-268,278-297`
  - CV 类型到 H5 类型的映射逻辑重复 3 次
  - 应提取为 `cvTypeToH5Type()` 辅助函数
- [ ] **HDF5 资源清理代码重复** — `FormatConversion.cpp` 多处
  - 每个 H5 函数都有手动 `H5Dclose`/`H5Sclose`/`H5Fclose`/`H5Tclose`
  - 建议使用 RAII 包装器或 `goto cleanup` 模式

### P2 — 建议

- [ ] **`read_POD` 中 OSV 分量读取重复 6 次** — `FormatConversion.cpp:1828-1899`
  - 读取 X/Y/Z/VX/VY/VZ 的代码结构完全相同
  - 应提取为 `readOsvComponent()` 辅助函数
- [ ] **SRTM 文件名格式化代码重复 12+ 次** — `FormatConversion.cpp:10500-10693`
  - 应提取为 `formatSrtmFileName()` 辅助函数
- [ ] **`XMLFile` 类职责过重** — `include/FormatConversion.h`
  - 同时承担通用 XML 读写和传感器特定数据解析
  - 应将 `get_gcps_from_TSX` 等移至对应传感器读取器类
- [ ] **函数参数过多** — `FormatConversion.h` 多处
  - `XMLFile_add_interferometric_phase` 14 个参数、`XMLFile_add_denoise_14` 15 个参数
- [ ] **冗余的函数重载** — `FormatConversion.h:783-831`
  - `TSX2h5` 有 6 个重载，建议使用默认参数值代替
- [ ] **`read_slc_from_TSXcos` 中 `malloc` 分配大块内存** — `FormatConversion.cpp:913`
  - 对大图像可能分配数百 MB，且逐像素处理效率低
  - 建议使用 GDAL RasterIO 直接读取到目标 Mat
- [ ] **`GDALAllRegister`/`GDALDestroyDriverManager` 调用不当** — `FormatConversion.cpp:888,944`
  - 每次调用都注册和销毁驱动管理器，应只调用一次
- [ ] **`BurstIndices::operator=` 返回值应为引用** — `FormatConversion.h:1129`
  - 且应检查自赋值

### P3 — 优化

- [ ] **函数名拼写错误** — `FormatConversion.h` 多处
  - `creat_new_h5` → `create_new_h5`
  - `invalide` → `invalid`
  - `ployFit` → `polyFit`
  - 注：会影响外部调用，暂不修改
- [x] **注释与数据不匹配** — `FormatConversion.cpp:7865-7888`
  - `Copy_para_from_h5_2_h5` 中多个数据集注释全部写成"最近斜距"，实际是地理坐标
  - 修复：将 8 处"最近斜距"注释改为正确的"左上角经度"、"左上角纬度"等
- [x] **`createVandermondeMatrix` 文档返回值描述反了** — `FormatConversion.h:1064`
  - 注释写"成功返回-1，否则返回0"，与项目惯例相反
  - 修复：改为"成功返回0，否则返回-1"
- [x] **无操作语句** — `FormatConversion.cpp:8046,8078,8110,8142`
  - `status;` 单独的表达式语句，无任何效果
  - 修复：注释掉 4 处无操作语句，添加说明注释
- [ ] **`orbitStateVectors` 成员为 public** — `FormatConversion.h:1286-1287`
  - 破坏封装性
  - 注：改为 private 会影响外部调用，暂不修改
- [ ] **`Sentinel1Utils` 大量成员为 public** — `FormatConversion.h:1936-2026`
  - 注：改为 private 会影响外部调用，暂不修改
- [ ] **成员变量命名不一致** — 部分使用 `m_` 前缀，部分不使用
  - 注：改名会影响外部调用，暂不修改
- [x] **注释风格不统一** — Doxygen/传统 C/单行注释混用
  - 修复：将 95 处 `/*@brief` 风格注释统一为 `/** @brief` 风格
- [ ] **大量注释掉的代码残留** — 整个文件中散布
  - 注：用户要求保留，暂不删除

---

## SBAS

### P0 — 必须修复

（无）

### P1 — 重要

- [ ] **`SBAS_node` 使用 C 风格 `malloc/free` 管理内存** — `SBAS.cpp:39-152`
  - `neigh_edges` 使用 `malloc` 分配、`free` 释放，手动实现深拷贝
  - 应改为 `std::vector<int>`，消除手动内存管理
- [ ] **`writeDIMACS_temporal()` 与 `writeDIMACS_spatial()` 大量重复** — `SBAS.cpp:774-1221`
  - 前半段（统计残差点、边界三角形、写 DIMACS 头部）几乎完全相同
  - 应提取为公共 `writeDIMACS_common()` 辅助函数
- [ ] **`compute_spatialTemporal_residue()` 与 `compute_high_coherence_residue()` 高度重复** — `SBAS.cpp:528-772,1743-1794`
  - 共享相同的三角形循环结构、坐标提取和方向判断逻辑
  - 应提取公共骨架函数
- [ ] **`return_check` 与其他模块重复** — `SBAS.cpp:26-37`
  - 应提取到公共头文件

### P2 — 建议

- [ ] **`compute_high_coherence_residue_by_gradient()` 中边查找模式重复** — `SBAS.cpp:1832-2003`
  - 三角形三条边与端点的匹配关系在三个 if/else 分支中完全复制（每块约 60 行）
  - 应提取"查找某条边位于哪两个端点之间"为辅助函数
- [ ] **赋值运算符返回值而非引用** — `SBAS.h:22,98,161`
  - `SBAS_node`、`SBAS_edge`、`SBAS_triangle` 的 `operator=` 应返回引用
- [ ] **`SBAS_edge` 和 `SBAS_triangle` 无需手写拷贝构造和赋值** — `SBAS.h:76-174`
  - 全部是 POD 类型，编译器默认生成的即可
- [ ] **`Mat` 参数缺少 const 限定** — `SBAS.h` 多处
  - `write_spatialTemporal_node()` 的 `B_temporal` 和 `B_effect` 等输入参数应为 `const Mat&`
- [ ] **`readDIMACS` 使用 `double*` 而非引用** — `SBAS.h:348-349`
  - `obj_value` 建议使用 `double&`
- [ ] **`error_head[256]` 使用固定大小 char 数组** — `SBAS.h` 多处
  - 建议使用 `std::string`

### P3 — 优化

- [ ] **`SBAS_edge::isBoundry` 拼写错误** — `SBAS.h:57`
  - 应为 `isBoundary`
- [ ] **变量名 `xxxx`** — `SBAS.cpp:659`
  - 应命名为 `orbit_idx` 或 `clamped_idx`
- [ ] **变量名 `a, b, c` 含义不清** — `SBAS.cpp:2546-2549`
  - 建议使用 `coef_intercept`, `coef_row`, `coef_col`
- [ ] **`GET_NEXT_LINE` 宏缩进误导** — `SBAS.cpp:18-24`
  - `else` 分支缩进对齐错误
- [ ] **大量被注释掉的旧代码未清理** — `SBAS.cpp:596-758`（约 160 行）
- [ ] **注释拼写错误** — `SBAS.cpp:576`
  - "由于edge1处于end2和end2之间" 应为 "end2和end3之间"
- [ ] **固定大小栈缓冲区** — `SBAS.cpp:970`
  - `char str[256]` 用于 `sprintf`，路径过长可能溢出，建议使用 `snprintf` 或 `std::string`

---

## Dem

### P0 — 必须修复

（无）

### P1 — 重要

- [ ] **牛顿迭代核心代码重复 4 次** — `Dem.cpp:511-2636`
  - `dem_newton_iter()`、`dem_newton_iter_test()`、`dem_newton_iter_14()`、`dem_newton_iter_14_dualfreqpingpong()` 中约 150-200 行矩阵运算代码逐行复制
  - 应提取为 `newton_iter_core()` 私有方法
- [ ] **轨道零多普勒点查找代码重复 3 次** — `Dem.cpp:1117-1178,1690-1752,2247-2309`
  - 二分法查找零多普勒时间的代码完全相同
- [ ] **`phase2dem_newton_iter()` 参数过多（15 个）且按值传递** — `Dem.h:31-47`
  - 多个 `Mat` 按值传递导致不必要的深拷贝
  - 应改为 `const Mat&`
- [ ] **硬编码调试路径** — `Dem.cpp:1511,1795-1796`
  - `G:\tmp\error.bin`、`E:\working_dir\projects\software\InSAR\bin\KK2.h5`
  - 应立即删除
- [ ] **`return_check` 与其他模块重复** — `Dem.cpp:21-32`

### P2 — 建议

- [ ] **`volatile bool parallel_flag` 用于 OpenMP 错误处理** — `Dem.cpp:476,930,1472,2049,2598`
  - `volatile` 不保证多线程可见性，应使用 `std::atomic<bool>`
- [ ] **`mode` 参数使用魔法数字** — `Dem.cpp` 多处
  - 应使用枚举类型
- [ ] **平地相位加回代码重复** — `Dem.cpp:682-696,1190-1204`
  - 6 系数多项式平地相位加回逻辑在多处重复

### P3 — 优化

- [ ] **变量名 `xxxx`** — `Dem.cpp:659,662`
  - 应命名为 `orbit_idx`
- [ ] **函数名暗示临时测试版本** — `Dem.h`
  - `dem_newton_iter_test`、`dem_newton_iter_14` 仍暴露为公共 API
  - 应使用更具描述性的名称或标记为内部函数
- [ ] **`phase2dem_newton_iter()` 参数注释使用"参数N"** — `Dem.h:14-30`
  - 不符合其他函数的 Doxygen 风格
- [ ] **大量注释掉的调试代码** — `Dem.cpp:1021-1024,1225-1239,1799-1814,2319-2370`

---

## Evaluation

### P0 — 必须修复

（无）

### P1 — 重要

- [ ] **`Pos()` 中牛顿迭代代码与 Dem 模块第 4 次重复** — `Evaluation.cpp:800-1259`
  - 约 200 行完全相同的矩阵运算代码
  - 应调用共享的 `newton_iter_core()` 函数
- [ ] **轨道零多普勒点查找代码再次重复** — `Evaluation.cpp:551-741`
  - 与 Dem/Registration 中的版本完全相同
- [ ] **`PhasePreserve()` 中约 190 行被注释掉的旧代码** — `Evaluation.cpp:165-355`
  - 严重影响可读性，应清理
- [ ] **`return_check` 与其他模块重复** — `Evaluation.cpp:19-30`

### P2 — 建议

- [ ] **`FFT2()` 不应是公共接口** — `Evaluation.h:44`
  - 纯内部的频域插值辅助函数，应移到私有或匿名命名空间
- [ ] **`Pos()` 参数过多（9 个）且缺少 const** — `Evaluation.h:40-43`
- [ ] **`PhasePreserve()` 和 `Unwrap()` 的数据读取代码高度重复** — `Evaluation.cpp` 两处
  - 从 H5 文件读取主/辅星参数的代码几乎逐行相同

### P3 — 优化

- [ ] **错误消息函数名不匹配** — `Evaluation.cpp:475`
  - `Unwrap()` 中错误消息写成了 `"PhasePreserve(): input check failed!"`
- [ ] **`FFT2()` 中未使用的变量** — `Evaluation.cpp:755-757`
  - `master_max` 和 `slave_max` 被赋值但从未读取
- [ ] **`Evaluation.h` 缺少传统 include guard** — `Evaluation.h`
  - 仅有 `#pragma once`，与其他头文件风格不一致
- [ ] **`Pos()` 函数缺少文档注释** — `Evaluation.h:40-43`

---

## simulation

### P0 — 必须修复

- [ ] **`SLC_deramp_14` mode==4 中类型转换 bug** — `SLC_simulator.cpp:3051,3121,3190`
  - `if (slc2.type() != CV_32F) slc.convertTo(slc, CV_32F);`
  - 检查的是 slc2 类型，转换的却是 slc，复制粘贴导致的 bug
- [ ] **第一个 `generateSLC` 函数未写入 SLC 像素（半成品）** — `SLC_simulator.cpp:269-550`
  - 计算了 `imaging_time` 和 `slant_range`，但从未将结果累加到 `slc.re`/`slc.im`
  - SLC 图像全是零

### P1 — 重要

- [ ] **零多普勒搜索算法重复 13 处（~1000 行）** — `SLC_simulator.cpp` 多处
  - 第 384-468、694-778、781-865、1289-1373、1376-1460、1797-1881、1884-1968、2087-2175、2187-2264、2527-2613、2616-2693、3408-3495、3497-3575 行
  - 应提取为 `findZeroDopplerTime()` 辅助函数
- [ ] **去参考/加参考相位操作重复 12+ 处** — `SLC_simulator.cpp` 多处
  - 应提取为 `applyPhaseCorrection()` 辅助函数
- [ ] **文件过大：4400+ 行** — `SLC_simulator.cpp`
  - 建议按功能拆分

### P2 — 建议

- [ ] **`generateSLC` 最多 27 个参数** — `SLC_simulator.h:60-187`
  - 建议将成像参数封装为 `ImagingParams` 结构体
- [ ] **缺少 `const` 修饰** — `SLC_simulator.h` 多处
  - `stateVec`、`dem`、`mappedDEM` 等纯输入数据应为 `const Mat&`
- [ ] **`pingpong_MLE` 的 `demPath` 按值传递** — `SLC_simulator.h:409`
  - `string demPath` 应改为 `const string&`
- [ ] **`SLC_deramp` 与 `SLC_reramp` 接口完全相同但功能相反** — `SLC_simulator.h:243,289`
  - 建议合并为一个函数，增加 `bool addPhase` 参数
- [ ] **OMP 并行循环内创建临时 Mat 对象** — `SLC_simulator.cpp` 多处
  - 每次迭代创建 `Mat XYZ, LLH(1,3,CV_64F), tt`，百万级像素时性能严重下降
- [ ] **`conv2` 函数定义在 cpp 文件全局作用域** — `SLC_simulator.cpp:30-60`
  - 通用二维卷积函数应提取到 Utils 类中
- [ ] **`Utils util` 在每个函数中重复实例化** — `SLC_simulator.cpp:2044,2447,3355`
  - 实际只需调用 `ell2xyz()`，应使用 `Utils::ell2xyz()` 静态版本

### P3 — 优化

- [ ] **冗余的自赋值** — `SLC_simulator.cpp:333-334,638-639,1215-1216,1757-1758`
  - `num_block_row = num_block_row;` 无效，应删除
- [ ] **变量命名不规范** — `SLC_simulator.cpp` 多处
  - 大量单字母变量名 `ii`/`jj`/`iii`/`bb`/`aa` 在嵌套 3-4 层循环中极易混淆
  - 第 1059 行 `wright` 应为 `width`
- [ ] **`error_head` 使用 C 风格字符数组** — `SLC_simulator.h:412`
  - 建议使用 `std::string`
- [ ] **空的 `if` 块** — `SLC_simulator.cpp:479-483,879-881` 等
  - `if (条件) { } else { ... }` 建议反转条件消除空代码块
- [ ] **大量注释掉的调试代码** — `SLC_simulator.cpp:1276-1283,3826-3860,3968-4007,4043-4096`
  - 包含硬编码文件路径 `G:\tmp\...`

---

## include（公共头文件）

### P0 — 必须修复

（无）

### P1 — 重要

- [ ] **`Heap` 类严重内存问题** — `globalparam.h:7-9`
  - 构造时立即 `malloc` 3 个 1 亿元素数组（约 2GB）
  - 析构函数为空，不释放内存（内存泄漏）
  - `empty()` 返回 `size`，语义与 `std::vector::empty()` 完全相反
  - 应改用 `std::vector`，修正 `empty()` 语义
- [ ] **`Package.h` 职责过重** — `Package.h`
  - 同时承担常量定义、结构体定义、DLL 导出宏、OpenCV/OpenMP 配置
  - 建议拆分为 `Constants.h`、`SarTypes.h`
- [ ] **`ComplexMat.h` 中 `using namespace std` 污染全局命名空间** — `ComplexMat.h:11`
  - 已在 ComplexMat 模块中记录

### P2 — 建议

- [ ] **`PI` 等宏名称过于通用** — `Package.h:7-9`
  - 容易与其他库冲突，建议改为 `INSAR_PI`、`INSAR_SPEED_OF_LIGHT`
  - 或改用 `constexpr` 常量
- [ ] **结构体命名不一致** — `Package.h:19,58,97`
  - `Position`、`Velocity`(PascalCase) vs `OSV`(全大写缩写)
- [ ] **Position/Velocity/OSV 手写的拷贝构造和赋值运算符冗余** — `Package.h:19-144`
  - 对仅含 POD 类型的结构体，编译器默认版本已足够
  - `operator=` 应返回引用而非值

### P3 — 优化

- [ ] **`globalparam.h` 毫无内容** — `include/globalparam.h`
  - 仅一行空文件，建议删除或补充实际内容
- [ ] **头文件 include guard 冗余** — `Package.h`、`ComplexMat.h`、`Utils.h`、`SLC_simulator.h`
  - 同时使用 `#pragma once` 和 `#ifndef`，建议统一保留 `#pragma once`

---

## 跨模块共性问题

> 以下问题涉及多个工程，建议统一修复。

### P1 — 重要

- [ ] **`findZeroDopplerTime()` 需提取为共享工具函数**
  - 消除 simulation(13处)、Deflat(5处)、Registration(1处)、Evaluation(2处) 共约 1600 行重复代码
  - 建议放在 Utils 中
- [ ] **`newton_iter_core()` 需提取为共享方法**
  - 消除 Dem(4处)、Evaluation(1处) 共约 600 行重复代码
- [ ] **`return_check()`/`parallel_check()` 需提取到 `include/ErrorCheck.h`**
  - 在 Utils、Filter、Unwrap、Registration、Deflat、SBAS 六个模块中重复定义
- [ ] **删除 `parallel_flag_change()`（死代码）**
  - 按值传递 volatile 修改无效，在所有模块中均未被实际调用
- [ ] **`volatile bool` 改为 `std::atomic<bool>`**
  - 出现在 Filter、Unwrap、Dem、Utils 中
  - `volatile` 不保证多线程内存可见性

### P2 — 建议

- [ ] **统一 `PI` 常量使用**
  - Filter.cpp、Unwrap.cpp、Deflat.cpp、Utils.cpp 中多处手写 `3.1415926535`（精度不足）
  - 应统一使用 `Package.h` 中的 `PI` 宏
- [ ] **统一光速常量使用**
  - Deflat.cpp 中手写 `300000000.0`，应使用 `VEL_C`
- [ ] **删除所有硬编码调试路径**
  - Utils.cpp、Dem.cpp、Unwrap.cpp、SLC_simulator.cpp 中共 10+ 处
- [ ] **清理所有 `#if 0` 和注释掉的死代码**
  - Unwrap.cpp 约 670 行、SBAS.cpp 约 160 行、Evaluation.cpp 约 190 行、各模块散布

### P3 — 优化

- [ ] **统一函数名拼写修正**
  - `Boundry` → `Boundary`（Utils.h、SBAS.h）
  - `creat` → `create`（FormatConversion.h）
  - `quailty` → `quality`（Unwrap.h）
  - `ployFit` → `polyFit`（Utils.h、FormatConversion.h）
  - `defficiency` → `deficiency`（Utils.cpp、Registration.cpp）
  - `invalide` → `invalid`（FormatConversion.cpp）
  - `failded` → `failed`（Deflat.cpp）
- [ ] **统一赋值运算符返回引用**
  - ComplexMat、tri_node、triangle、tri_edge、node_index、SBAS_node、SBAS_edge、SBAS_triangle、Position、Velocity、OSV、BurstIndices
- [ ] **统一注释风格为 Doxygen**
