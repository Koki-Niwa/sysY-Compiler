#include "ToyIR.h"
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace toy {
std::string Cursor::peek() {
  if (cached_) return next_;
  while (pos_ < line_.size() && std::isspace(static_cast<unsigned char>(line_[pos_]))) ++pos_;
  if (pos_ == line_.size() || line_[pos_] == ';') return cached_ = true, next_ = "";
  const size_t start = pos_;
  char ch = line_[pos_++];
  if (ch == '@' && pos_ < line_.size() && line_[pos_] == '"') {
    ++pos_;
    while (pos_ < line_.size()) {
      if (line_[pos_] == '\\' && pos_ + 2 < line_.size()) { pos_ += 3; continue; }
      if (line_[pos_++] == '"') break;
    }
  } else if (ch != '[' && ch != ']' && ch != '(' && ch != ')' &&
             ch != ',' && ch != '=' && ch != '{' && ch != '}') {
    while (pos_ < line_.size()) {
      ch = line_[pos_];
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == '[' || ch == ']' ||
          ch == '(' || ch == ')' || ch == ',' || ch == '=' || ch == '{' ||
          ch == '}' || ch == ';') break;
      ++pos_;
    }
  }
  next_ = line_.substr(start, pos_ - start);
  cached_ = true;
  return next_;
}
std::string Cursor::take() { std::string s = peek(); cached_ = false; return s; }
bool Cursor::accept(const std::string& token) {
  if (peek() != token) return false;
  take(); return true;
}
void Cursor::expect(const std::string& token) {
  std::string got = take();
  if (got != token) throw std::runtime_error("expected '" + token + "', got '" + got + "'");
}
void Cursor::end() {
  if (!peek().empty()) throw std::runtime_error("unexpected trailing token '" + peek() + "'");
}
static uint64_t decimal(const std::string& s) {
  if (s.empty() || s[0] == '-') throw std::runtime_error("expected non-negative decimal integer");
  size_t index = 0;
  uint64_t v = std::stoull(s, &index, 10);
  if (index != s.size()) throw std::runtime_error("invalid decimal integer: " + s);
  return v;
}
Type parseType(Cursor& c) {
  std::string name = c.take();
  if (name == "[") {
    uint64_t count = decimal(c.take());
    c.expect("x");
    Type elem = parseType(c);
    c.expect("]");
    if (elem.size && count > std::numeric_limits<uint64_t>::max() / elem.size)
      throw std::runtime_error("aggregate type size overflow");
    return {"array", count * elem.size, count, {std::move(elem)}};
  }
  if (name == "void") return {name, 0, 0, {}};
  if (name == "i1" || name == "i8") return {name, 1, 0, {}};
  if (name == "i32" || name == "float") return {name, 4, 0, {}};
  if (name == "i64" || name == "double" || name == "ptr") return {name, 8, 0, {}};
  throw std::runtime_error("unsupported LLVM type: '" + name + "'");
}
Value parseValue(Cursor& c, Type type) {
  std::string text = c.take();
  if (text.empty()) throw std::runtime_error("expected " + type.name + " value");
  if (text[0] == '%' || text[0] == '@') {
    if (text[0] == '@') symbolName(text);
  } else if (text == "null") {
    if (type.name != "ptr") throw std::runtime_error("null must have ptr type");
  } else if (type.name == "float" || type.name == "double") {
    (void)doubleBits(text);
  } else if (type.isInteger()) {
    if (text != "true" && text != "false") {
      size_t n = 0; (void)std::stoll(text, &n, 0);
      if (n != text.size()) throw std::runtime_error("invalid integer constant " + text);
    } else if (type.name != "i1") throw std::runtime_error("boolean constant requires i1");
  } else {
    throw std::runtime_error("invalid value for type " + type.name);
  }
  return {std::move(type), text};
}
std::string symbolName(const std::string& token) {
  if (token.size() < 2 || token[0] != '@')
    throw std::runtime_error("expected @symbol");
  if (token[1] != '"') return token.substr(1);
  if (token.back() != '"' || token.size() < 4)
    throw std::runtime_error("malformed quoted LLVM symbol");
  std::string result;
  for (size_t i = 2; i + 1 < token.size(); ++i) {
    if (token[i] != '\\') { result += token[i]; continue; }
    if (i + 2 >= token.size() - 1)
      throw std::runtime_error("malformed LLVM \\XX symbol escape");
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    int a = hex(token[++i]), b = hex(token[++i]);
    if (a < 0 || b < 0) throw std::runtime_error("invalid LLVM symbol hex escape");
    result += static_cast<char>((a << 4) | b);
  }
  return result;
}
std::string gasSymbol(const std::string& name) {
  if (name.empty()) throw std::runtime_error("empty symbol");
  bool bare = std::isalpha(static_cast<unsigned char>(name[0])) ||
              name[0] == '_' || name[0] == '.' || name[0] == '$';
  for (unsigned char c : name)
    bare &= std::isalnum(c) || c == '_' || c == '.' || c == '$';
  if (bare) return name;
  std::string out = "\"";
  for (unsigned char c : name) {
    if (c == '"' || c == '\\') out += '\\';
    if (c < 32 || c == 127)
      throw std::runtime_error("symbol with control byte cannot be represented by GAS");
    out += static_cast<char>(c);
  }
  return out + "\"";
}
uint64_t doubleBits(const std::string& s) {
  if (s.size() == 18 && s.substr(0, 2) == "0x") {
    size_t n = 0;
    uint64_t bits = std::stoull(s.substr(2), &n, 16);
    if (n != 16) throw std::runtime_error("bad LLVM hexadecimal float");
    return bits;
  }
  size_t n = 0;
  double d = std::stod(s, &n);
  if (n != s.size()) throw std::runtime_error("bad float literal");
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof(bits));
  return bits;
}
uint32_t floatBits(const std::string& s) {
  uint64_t dbl = doubleBits(s);
  double d;
  std::memcpy(&d, &dbl, sizeof(d));
  float f = static_cast<float>(d);
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}
std::string jsonQuote(const std::string& s) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out << '\\' << c; continue; }
    if (c == '\n') { out << "\\n"; continue; }
    if (c == '\r') { out << "\\r"; continue; }
    if (c == '\t') { out << "\\t"; continue; }
    if (c < 32) {
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
    } else out << c;
  }
  return out.str() + '"';
}
// Standalone SHA-256: no LLVM, OpenSSL, platform cryptography or shell dependency.
std::string sha256File(const std::string& path) {
  static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  auto rotate = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read file for sha256: " + path);
  std::vector<uint8_t> data(std::istreambuf_iterator<char>(in), {});
  uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(bitlen >> (i * 8)));
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  for (size_t offset = 0; offset < data.size(); offset += 64) {
    uint32_t w[64] = {};
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 4; ++j) w[i] = (w[i] << 8) | data[offset + i * 4 + j];
    for (int i = 16; i < 64; ++i) {
      uint32_t a = w[i-15], b = w[i-2];
      w[i] = w[i-16] + (rotate(a,7)^rotate(a,18)^(a>>3)) +
             w[i-7] + (rotate(b,17)^rotate(b,19)^(b>>10));
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
      uint32_t choice=(e&f)^(~e&g);
      uint32_t t1=hh+s1+choice+k[i]+w[i];
      uint32_t s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
      uint32_t maj=(a&b)^(a&c)^(b&c);
      uint32_t t2=s0+maj;
      hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    uint32_t v[8]={a,b,c,d,e,f,g,hh};
    for(int i=0;i<8;++i)h[i]+=v[i];
  }
  std::ostringstream out;
  for(uint32_t v:h)out<<std::hex<<std::setfill('0')<<std::setw(8)<<v;
  return out.str();
}
}  // namespace toy
