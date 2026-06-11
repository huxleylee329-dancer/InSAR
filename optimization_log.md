# InSAR 项目代码整合、编译修复与几何对齐优化日志

本日志详细记录了在 `merge-clean` 分支中，逐步集成 external DLL 功能、修复编译错误、清理代码警告以及修正浮点精度对齐 Bug 的全过程。

---

## 历史提交与修复概览（当前分支已完成部分）

| 整合来源 (Commit) | 日期 | 作者 | 涉及模块 | 问题/修改描述 |
| :--- | :--- | :--- | :--- | :--- |
| `工作区现场修改` | 2026-06-11 | lewis / AI | FormatConversion, Utils, Registration, Deflat, simulation | 1. 修复由于引入 C++ GDAL API 导致的头文件缺失与编译错误。<br>2. 清理未引用局部变量（保留注释以利 Review）。<br>3. **采用 cvRound 四舍五入彻底消除浮点微差导致的几何对齐偏一像素隐患。** |
| `27e9894a` | 2026-06-07 | lewis | ComplexMat | 修复 `insar_ui` 项目在部分编译器下的 C++ 标准库头文件编译错误。 |
| `801f47c1` | 2026-06-07 | lewis | FormatConversion | 解决 TinyXML DLL 接口污染/泄露问题，重构为 Pimpl 模式并增加进度回调。 |
| `a14aef4d` | 2026-06-04 | lewis | FormatConversion | 修复项目工程文件中 zlib 库依赖名称拼写错误导致的链接失败。 |
| `0fda7970` | 2026-06-03 | lewis | 全模块 | 全局清理编译器警告（如未使用的变量、隐式类型转换警告等）。 |
| `28da79da` | 2026-06-03 | lewis | 全模块 | 统一全平台源码文件为 UTF-8 编码，并规范 DLL 模块的 `dllmain.cpp` 入口。 |
| `0ac49301` | 2026-06-02 | lewis | FormatConversion, Utils | 修复干涉图生成时因 XML 属性缺失引发的潜在空指针崩溃，优化坐标转换参数传递。 |

---

## 详细修改记录

### 1. 编译错误现场修复 (FormatConversion)
针对 `FormatConversion` 项目在 Debug/Release 配置下发生的编译阻碍，进行了如下修复：
- **`_mkdir` 找不到标识符**：包含 `<direct.h>` 头文件以提供 Windows 平台下的文件夹创建函数声明。
- **GDAL C++ 接口类未声明**：解开 `FormatConversion.cpp` 头部被注释的 `#include "gdal_priv.h"`，为 `GDALDataset` 和 `GDALRasterBand` 提供正确的 C++ 声明。
- **`InitializeGDALOnce` 找不到标识符**：在 `FormatConversion.cpp` 头部引入 `<mutex>` 并定义懒加载初始化函数 `InitializeGDALOnce`：
  ```cpp
  static std::once_flag g_gdal_init_flag;
  static void InitializeGDALOnce()
  {
  	std::call_once(g_gdal_init_flag, [](){
  		GDALAllRegister();
  	});
  }
  ```
  保证了 GDAL 驱动安全且单次注册，解决了 `geotiffread` 内的调用报错。

### 2. 图像几何对齐精度修复 — cvRound 替代直接截断 (Utils, Deflat & simulation)
- **发现的问题**：
  - **Utils 模块**：在十多个 Copernicus DEM 裁剪与行列数计算分支中（第 9060 ~ 10803 行），代码频繁使用浮点数除法计算像素索引与行列数：
    `int rows = (latMax - latMin) / latSpacing;`
  - **Deflat 模块**：在雷达几何反投影计算中（第 1739、2005 行等），像素的方位向和距离向索引计算：
    `int azimuthIndex = (zeroDopplerTime - acquisitionStartTime) / time_interval;`
  - **simulation 模块**：在 SLC 模拟计算中（第 485、806 行等），雷达影像中对应坐标的计算原先显式使用了 `floor` 截断：
    `int azimuthIndex = floor((zeroDopplerTime - acquisitionStartTime) / time_interval);`
  
  上述浮点数运算在 C++ 隐式类型转换下均会采用 **向零截断** 或 **向下取整** 机制。若计算结果由于浮点微差变成 `14.9999999998`，截断将导致其变为 `14`，在地理网格裁剪对齐及雷达影像定位中，会引发**“刚好偏了一像素/一行”的经典对齐 Bug**。
- **解决办法 (方案 B)**：
  将上述所有除法与截断计算全部替换为 OpenCV 的 **`cvRound`** 函数以实现**四舍五入对齐**。例如：
  ```cpp
  int rows = cvRound((latMax - latMin) / latSpacing);
  int azimuthIndex = cvRound((zeroDopplerTime - acquisitionStartTime) / time_interval);
  ```
  这消除了浮点精度带来的行列数和网格裁剪范围 of DEM 的误差，大幅提升了几何对齐精度。
- **遗留 static_cast 隐患的全面清理**：
  在对警告和对齐精度的全局排查中，我们发现部分代码由于早期为消除 `C4244` 精度警告，简单使用了 `static_cast<int>` 强转。这其实仅是将“向零截尾”显式化，仍会产生对齐偏一像素的隐患。本次已对其进行了彻底的全局升级：
  - **`Deflat.cpp`**：将第 1034~1035 行雷达方位/距离索引、第 3143~3590 行全部 6 处 Copernicus DEM 裁剪的 `static_cast<int>` 均升级为了 `cvRound`。
  - **`FormatConversion.cpp`**：将第 11328~11329 行的行列数计算、第 11369~11816 行全部 6 处 Copernicus DEM 裁剪、第 11836~11837 行的 `getElevation` 图像网格行列坐标强转均升级为了 `cvRound`。
- **其它数学取整安全转换**：
  对第 11042 ~ 11051 行中本已完成 `ceil`/`floor` 运算并赋值给 `int` 的经纬度区间计算，使用 `static_cast<int>` 显式消除精度截断警告，该处范围明确，为 100% 安全转换。

### 3. 未引用局部变量清理 — 采用注释保留机制 (FormatConversion, Utils, Registration & Deflat)
To resolve `warning C4101` (unused local variables) while preserving historical context for code reviews, we commented out unused code using the `/*...*/` format:
- **`FormatConversion.cpp`**：
  - 第 13883 行、14257 行的 `s`：
    `int ret, year, month, day, hour, minute, second/*, s*/;`
  - 第 14046 行、14447 行的 `pchild1`（注意：经核对 `pchild` 是被使用的变量，故在此处正常保留其活动声明）：
    `TiXmlElement* pnode, * pchild/*, * pchild1*/;`
- **`Utils.cpp`**：
  - 第 9651、9776、10004、10276、10452 行的 `temp`：
    `int xx[...] , yy[...]/*, temp*/;`
  - 第 11113 行 the `ret`：
    `/*int ret;*/`
- **`Registration.cpp`**：
  - 第 1573 行的 `ix`、`iy`、`delta`（由于后续 outliers 剔除的 OMP 循环被整体注释已成为死代码）：
    `int /*ix, iy, */count = 0, c = 0; double /*delta, */thresh = 2.0;`
- **`Deflat.cpp`**：
  - 第 1650、1916 行的 `ret`：
    `/*int ret;*/`
  - 第 1761、2027 行的 `up_count`、`down_count` 等插值辅助变量：
    `int up, down, left, right/*, up_count, ...*/;`
  - 第 2321~2323 行在 `SLC_deramp` 函数中冗余声明的 `lonMin`、`nearRangeTime`、`offset_col` 等十个未被使用的局部变量：
    使用注释屏蔽声明。
- **`SLC_simulator.cpp`**：
  - 第 659 行的 `ret`：
    `/*int ret;*/`

### 4. 潜在崩溃与内存错误修复 (Utils)
- **`fprintf` 格式化 %s 传参错误**：
  在 `Utils.cpp` 第 15467 行，原有代码直接将 `std::string` 传给了带有 `%s` 的 `fprintf`。在 x64 等运行环境下会产生垃圾字符输出甚至直接内存崩溃。
  修改为调用 `.c_str()` 以保证类型安全：
  ```cpp
  fprintf(stderr, "无法打开 Geoid 文件: %s\n", geoidFilePath.c_str());
  ```

---
*注：本分支仅针对目前已合入的代码与编译警告进行了上述清理。对于 master 上其他未合入的全局优化与并发改造（如 HDF5 Concurrency Mutex、范德模拟合去重等），在本分支的代码中暂不列入，待后续优化重排时统一记录。*
