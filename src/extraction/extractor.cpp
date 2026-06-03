/**
 * extractor.cpp — 语言提取器的注册入口
 *
 * 本文件是 extraction 模块的"桩文件"，当前不包含实际逻辑。
 * 真正的提取实现在 cpp_extractor.cpp 中（C++ 和 Python）。
 *
 * 保留此文件的目的是：
 *   1. 作为 LanguageExtractor 基类的稳定编译单元
 *   2. 未来可扩展其他语言提取器（如 Rust、Go）时在此注册
 *   3. 保持 CMake 中 extraction 库的目标完整性
 */

#include "codegraph/extraction/extractor.h"

namespace codegraph {

// Extraction entry points live in cpp_extractor.cpp. This translation unit is
// intentionally kept as the stable home for future non-language-specific
// extraction helpers, without carrying an alternate indexing implementation.

}  // namespace codegraph
