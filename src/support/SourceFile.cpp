#include "support/SourceFile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "support/Errors.h"

namespace sysy {

namespace {

// 行尾规范化：\r\n → \n；孤立 \r → \n。其余字节原样保留（含 UTF-8 与 NUL）。
std::string normalizeNewlines(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '\r') {
      out.push_back('\n');
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;   // 吃掉 \r\n 里的 \n
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

void SourceFile::build(std::string content) {
  text_ = normalizeNewlines(content);
  // 保证以 '\n' 结尾：让"最后一行没有换行符"与"有换行符"两种情况
  // 在 offsetOf/lineText 上表现一致（诊断输出更稳）。
  if (text_.empty() || text_.back() != '\n') text_.push_back('\n');
  end_ = text_.size();

  // lineStart_ = 每条逻辑行的起点，末尾再放一个哨兵 (= text_.size())。
  // 注意：行尾的 '\n' 只作为【行终止符】，不算下一行，所以"末尾换行"不会多出一行。
  lineStart_.clear();
  lineStart_.push_back(0);
  for (size_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\n' && i + 1 < text_.size()) {
      lineStart_.push_back(static_cast<uint32_t>(i + 1));
    }
  }
  if (lineStart_.back() != end_) lineStart_.push_back(static_cast<uint32_t>(end_));
}

SourceFile SourceFile::load(const std::string& path) {
  SourceFile f;
  f.path_ = path;
  f.build(readFileOrThrow(path));
  return f;
}

SourceFile SourceFile::fromString(const std::string& path, std::string content) {
  SourceFile f;
  f.path_ = path;
  f.build(std::move(content));
  return f;
}

size_t SourceFile::offsetOf(SourceLoc loc) const {
  if (loc.line == 0 || loc.line > lineCount()) return text_.size();
  if (loc.col == 0) return lineStart_[loc.line - 1];
  const size_t start = lineStart_[loc.line - 1];
  const size_t end = lineStart_[loc.line];                 // 下一行起点（含行尾 '\n'）
  size_t lineLen = end - start;
  while (lineLen > 0 && text_[start + lineLen - 1] == '\n') --lineLen;   // 去掉行终止符
  const size_t off = start + (loc.col - 1);
  return off > start + lineLen ? start + lineLen : off;    // 列越界 → 夹到该行末尾
}

SourceLoc SourceFile::locOf(size_t offset) const {
  if (text_.empty()) return SourceLoc(1, 1);
  if (offset >= text_.size()) offset = text_.size() - 1;
  // 找最后一个 lineStart_ <= offset
  const auto it = std::upper_bound(lineStart_.begin(), lineStart_.end(),
                                   static_cast<uint32_t>(offset));
  size_t idx = static_cast<size_t>(it - lineStart_.begin());   // >= 1
  if (idx > lineCount()) idx = lineCount();                    // offset 落在末尾哨兵上
  const size_t start = lineStart_[idx - 1];
  return SourceLoc(static_cast<uint32_t>(idx), static_cast<uint32_t>(offset - start + 1));
}

std::string SourceFile::lineText(uint32_t line) const {
  if (line == 0 || line > lineCount()) return std::string();
  const size_t start = lineStart_[line - 1];
  size_t lineLen = lineStart_[line] - start;
  while (lineLen > 0 && text_[start + lineLen - 1] == '\n') --lineLen;   // 去行终止符
  return text_.substr(start, lineLen);
}

std::string SourceFile::slice(size_t start, size_t end) const {
  if (start > text_.size()) start = text_.size();
  if (end > text_.size()) end = text_.size();
  if (end < start) end = start;
  return text_.substr(start, end - start);
}

// —— 文件读取（二进制模式：不做任何平台相关的换行转换）——
std::string readFileOrThrow(const std::string& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec))
    throw IOError("'" + path + "' is a directory, not a source file");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw IOError("cannot open input file '" + path + "'");
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) throw IOError("error while reading '" + path + "'");
  return buf.str();
}

}  // namespace sysy
