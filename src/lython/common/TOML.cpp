#include "TOML.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Error.h"

#include <cassert>
#include <limits>

namespace lython::toml {

// --- Value -------------------------------------------------------------------

Value::Value(Value &&) noexcept = default;
Value &Value::operator=(Value &&) noexcept = default;
Value::~Value() = default;

Value Value::string(std::string text) {
  Value value(Kind::String);
  value.storage = std::move(text);
  return value;
}

Value Value::integer(std::int64_t number) {
  Value value(Kind::Integer);
  value.storage = number;
  return value;
}

Value Value::floating(double number) {
  Value value(Kind::Float);
  value.storage = number;
  return value;
}

Value Value::boolean(bool truth) {
  Value value(Kind::Boolean);
  value.storage = truth;
  return value;
}

Value Value::dateTime(Kind kind, std::string spelling) {
  assert((kind == Kind::OffsetDateTime || kind == Kind::LocalDateTime ||
          kind == Kind::LocalDate || kind == Kind::LocalTime) &&
         "not a date-time kind");
  Value value(kind);
  value.storage = std::move(spelling);
  return value;
}

Value Value::array() {
  Value value(Kind::Array);
  value.storage = std::make_unique<Array>();
  return value;
}

Value Value::table() {
  Value value(Kind::Table);
  value.storage = std::make_unique<Table>();
  return value;
}

bool Value::isDateTime() const {
  return valueKind == Kind::OffsetDateTime ||
         valueKind == Kind::LocalDateTime || valueKind == Kind::LocalDate ||
         valueKind == Kind::LocalTime;
}

const std::string *Value::asString() const {
  return valueKind == Kind::String ? &std::get<std::string>(storage) : nullptr;
}

std::optional<std::int64_t> Value::asInteger() const {
  if (valueKind != Kind::Integer)
    return std::nullopt;
  return std::get<std::int64_t>(storage);
}

std::optional<double> Value::asFloat() const {
  if (valueKind != Kind::Float)
    return std::nullopt;
  return std::get<double>(storage);
}

std::optional<bool> Value::asBoolean() const {
  if (valueKind != Kind::Boolean)
    return std::nullopt;
  return std::get<bool>(storage);
}

const std::string *Value::asDateTime() const {
  return isDateTime() ? &std::get<std::string>(storage) : nullptr;
}

const Array *Value::asArray() const {
  return valueKind == Kind::Array
             ? std::get<std::unique_ptr<Array>>(storage).get()
             : nullptr;
}

Array *Value::asArray() {
  return valueKind == Kind::Array
             ? std::get<std::unique_ptr<Array>>(storage).get()
             : nullptr;
}

const Table *Value::asTable() const {
  return valueKind == Kind::Table
             ? std::get<std::unique_ptr<Table>>(storage).get()
             : nullptr;
}

Table *Value::asTable() {
  return valueKind == Kind::Table
             ? std::get<std::unique_ptr<Table>>(storage).get()
             : nullptr;
}

llvm::StringRef Value::kindName(Kind kind) {
  switch (kind) {
  case Kind::String:
    return "string";
  case Kind::Integer:
    return "integer";
  case Kind::Float:
    return "float";
  case Kind::Boolean:
    return "boolean";
  case Kind::OffsetDateTime:
    return "offset date-time";
  case Kind::LocalDateTime:
    return "local date-time";
  case Kind::LocalDate:
    return "local date";
  case Kind::LocalTime:
    return "local time";
  case Kind::Array:
    return "array";
  case Kind::Table:
    return "table";
  }
  return "value";
}

// --- Table -------------------------------------------------------------------

const Value *Table::find(llvm::StringRef key) const {
  auto found = index.find(key);
  return found == index.end() ? nullptr : &entries[found->second].second;
}

Value *Table::find(llvm::StringRef key) {
  auto found = index.find(key);
  return found == index.end() ? nullptr : &entries[found->second].second;
}

Value &Table::insert(std::string key, Value value) {
  assert(!contains(key) && "key already present");
  index[key] = entries.size();
  entries.emplace_back(std::move(key), std::move(value));
  return entries.back().second;
}

const Value *Table::lookup(llvm::ArrayRef<llvm::StringRef> path) const {
  const Table *table = this;
  const Value *value = nullptr;
  for (llvm::StringRef key : path) {
    if (!table)
      return nullptr;
    value = table->find(key);
    if (!value)
      return nullptr;
    table = value->asTable();
  }
  return value;
}

// --- Parser ------------------------------------------------------------------

namespace {

// How a table came to exist, which decides what may extend it later
// (toml.io/en/v1.0.0#table): a header may define a table only once, dotted
// keys may extend only the tables dotted keys created, and an inline table is
// complete as written.
enum class Origin {
  // Named on the way to a header (`a` in `[a.b]`); a later `[a]` defines it.
  Implicit,
  // Defined by a `[header]` or as an element of `[[header]]`.
  Header,
  // Created by a dotted key (`a.b = 1` creates `a`).
  Dotted,
  // An inline table or a table inside one.
  Inline,
};

class Parser {
public:
  Parser(llvm::StringRef text, unsigned maxDepth)
      : text(text), maxDepth(maxDepth) {}

  ParseResult run();

private:
  llvm::StringRef text;
  std::size_t position = 0;
  // Arrays and inline tables nest by recursion; deeper than this is refused
  // rather than allowed to exhaust the stack.
  unsigned maxDepth;
  unsigned depth = 0;
  std::optional<ParseError> error;
  // The document's table, which headers are resolved from.
  Table *root = nullptr;
  llvm::DenseMap<const Table *, Origin> origins;
  // Arrays a `[[header]]` created: the only arrays a header may append to.
  llvm::DenseSet<const Array *> tableArrays;

  bool atEnd() const { return position >= text.size(); }
  char peek(std::size_t ahead = 0) const {
    return position + ahead < text.size() ? text[position + ahead] : '\0';
  }
  bool startsWith(llvm::StringRef prefix) const {
    return text.substr(position).starts_with(prefix);
  }

  bool fail(const llvm::Twine &message) { return failAt(position, message); }
  bool failAt(std::size_t offset, const llvm::Twine &message);

  bool validateEncoding();
  void skipWhitespace();
  bool atNewline() const;
  bool skipNewline();
  bool skipComment();
  bool skipWhitespaceCommentsAndNewlines();
  bool expectEndOfLine();

  bool parseKey(llvm::SmallVectorImpl<std::string> &key);
  bool parseSimpleKey(std::string &key);
  bool parseHeader(Table *&current);
  bool parseKeyValue(Table &table);
  bool assignDotted(Table &table, llvm::ArrayRef<std::string> key,
                    std::size_t keyOffset, Value value);

  bool parseValue(std::optional<Value> &value);
  bool parseBasicString(std::string &out);
  bool parseMultilineBasicString(std::string &out);
  bool parseLiteralString(std::string &out);
  bool parseMultilineLiteralString(std::string &out);
  bool parseEscape(std::string &out);
  bool parseArray(std::optional<Value> &value);
  bool parseInlineTable(std::optional<Value> &value);
  bool parseScalarToken(std::optional<Value> &value);
  bool parseNumber(llvm::StringRef token, std::size_t offset,
                   std::optional<Value> &value);
  bool parseDateTime(llvm::StringRef token, std::size_t offset,
                     std::optional<Value> &value);

  void freezeInline(const Table &table);
};

bool isControl(unsigned char c) {
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

bool isBareKeyChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isHexDigit(char c) {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void appendUTF8(std::string &out, std::uint32_t codePoint) {
  char buffer[4];
  char *cursor = buffer;
  bool converted = llvm::ConvertCodePointToUTF8(codePoint, cursor);
  assert(converted && "code point validated before encoding");
  (void)converted;
  out.append(buffer, cursor);
}

bool Parser::failAt(std::size_t offset, const llvm::Twine &message) {
  if (error)
    return false;
  ParseError found;
  found.line = 1;
  found.column = 1;
  for (std::size_t index = 0; index < offset && index < text.size(); ++index) {
    unsigned char c = static_cast<unsigned char>(text[index]);
    if (c == '\n') {
      ++found.line;
      found.column = 1;
    } else if ((c & 0xc0) != 0x80) {
      // A UTF-8 lead byte or ASCII: one more scalar value on the line.
      ++found.column;
    }
  }
  found.message = message.str();
  error = std::move(found);
  return false;
}

bool Parser::validateEncoding() {
  const auto *begin = reinterpret_cast<const llvm::UTF8 *>(text.data());
  const auto *cursor = begin;
  const auto *end = begin + text.size();
  if (llvm::isLegalUTF8String(&cursor, end))
    return true;
  return failAt(static_cast<std::size_t>(cursor - begin),
                "the document is not valid UTF-8");
}

void Parser::skipWhitespace() {
  while (peek() == ' ' || peek() == '\t')
    ++position;
}

bool Parser::atNewline() const {
  return peek() == '\n' || (peek() == '\r' && peek(1) == '\n');
}

bool Parser::skipNewline() {
  if (peek() == '\n') {
    ++position;
    return true;
  }
  if (peek() == '\r' && peek(1) == '\n') {
    position += 2;
    return true;
  }
  return false;
}

bool Parser::skipComment() {
  if (peek() != '#')
    return true;
  while (!atEnd() && !atNewline()) {
    if (isControl(static_cast<unsigned char>(peek())))
      return fail("control character in a comment");
    ++position;
  }
  return true;
}

bool Parser::skipWhitespaceCommentsAndNewlines() {
  while (true) {
    skipWhitespace();
    if (!skipComment())
      return false;
    if (!skipNewline())
      return true;
  }
}

bool Parser::expectEndOfLine() {
  skipWhitespace();
  if (!skipComment())
    return false;
  if (atEnd() || skipNewline())
    return true;
  if (peek() == '\r')
    return fail("a carriage return must be followed by a line feed");
  return fail("expected the end of the line");
}

bool Parser::parseSimpleKey(std::string &key) {
  if (startsWith("\"\"\"") || startsWith("'''"))
    return fail("a key cannot be a multi-line string");
  if (peek() == '"')
    return parseBasicString(key);
  if (peek() == '\'')
    return parseLiteralString(key);
  std::size_t start = position;
  while (isBareKeyChar(peek()))
    ++position;
  if (position == start)
    return fail("expected a key");
  key = text.slice(start, position).str();
  return true;
}

bool Parser::parseKey(llvm::SmallVectorImpl<std::string> &key) {
  while (true) {
    std::string part;
    if (!parseSimpleKey(part))
      return false;
    key.push_back(std::move(part));
    std::size_t afterPart = position;
    skipWhitespace();
    if (peek() != '.') {
      position = afterPart;
      return true;
    }
    ++position;
    skipWhitespace();
  }
}

bool Parser::parseHeader(Table *&current) {
  std::size_t headerOffset = position;
  bool isArray = startsWith("[[");
  position += isArray ? 2 : 1;
  skipWhitespace();
  llvm::SmallVector<std::string, 4> key;
  if (!parseKey(key))
    return false;
  skipWhitespace();
  if (isArray ? !startsWith("]]") : peek() != ']')
    return fail(isArray ? "expected ']]' to close the table array header"
                        : "expected ']' to close the table header");
  position += isArray ? 2 : 1;

  Table *table = root;
  for (std::size_t index = 0; index + 1 < key.size(); ++index) {
    Value *value = table->find(key[index]);
    if (!value) {
      Table *created = table->insert(key[index], Value::table()).asTable();
      origins[created] = Origin::Implicit;
      table = created;
      continue;
    }
    if (Array *array = value->asArray()) {
      if (!tableArrays.contains(array))
        return failAt(headerOffset, llvm::Twine("'") + key[index] +
                                        "' is a static array; a header "
                                        "cannot reach into it");
      table = array->back().asTable();
      continue;
    }
    Table *existing = value->asTable();
    if (!existing)
      return failAt(headerOffset, llvm::Twine("'") + key[index] + "' is already defined, as a value of type " +
                                      Value::kindName(value->kind()));
    if (origins.lookup(existing) == Origin::Inline)
      return failAt(headerOffset, llvm::Twine("'") + key[index] +
                                      "' is an inline table, which cannot be "
                                      "extended");
    table = existing;
  }

  const std::string &last = key.back();
  Value *value = table->find(last);
  if (isArray) {
    if (!value) {
      Array *array = table->insert(last, Value::array()).asArray();
      tableArrays.insert(array);
      value = table->find(last);
    }
    Array *array = value->asArray();
    if (!array || !tableArrays.contains(array))
      return failAt(headerOffset, llvm::Twine("'") + last + "' is already defined, as a " +
                                      (array ? llvm::StringRef("static array")
                                             : Value::kindName(value->kind())));
    array->push_back(Value::table());
    current = array->back().asTable();
    origins[current] = Origin::Header;
    return true;
  }
  if (!value) {
    current = table->insert(last, Value::table()).asTable();
    origins[current] = Origin::Header;
    return true;
  }
  Table *existing = value->asTable();
  if (!existing || origins.lookup(existing) != Origin::Implicit)
    return failAt(headerOffset, llvm::Twine("'") + last + "' is already defined");
  origins[existing] = Origin::Header;
  current = existing;
  return true;
}

bool Parser::assignDotted(Table &table, llvm::ArrayRef<std::string> key,
                          std::size_t keyOffset, Value value) {
  Table *target = &table;
  for (std::size_t index = 0; index + 1 < key.size(); ++index) {
    Value *existing = target->find(key[index]);
    if (!existing) {
      Table *created = target->insert(key[index], Value::table()).asTable();
      origins[created] = Origin::Dotted;
      target = created;
      continue;
    }
    Table *existingTable = existing->asTable();
    if (!existingTable)
      return failAt(keyOffset, llvm::Twine("'") + key[index] + "' is already defined, as a value of type " +
                                   Value::kindName(existing->kind()));
    if (origins.lookup(existingTable) != Origin::Dotted)
      return failAt(keyOffset, llvm::Twine("'") + key[index] +
                                   "' is a table defined elsewhere; dotted "
                                   "keys cannot extend it");
    target = existingTable;
  }
  if (target->contains(key.back()))
    return failAt(keyOffset, llvm::Twine("'") + key.back() + "' is already defined");
  target->insert(key.back(), std::move(value));
  return true;
}

bool Parser::parseKeyValue(Table &table) {
  std::size_t keyOffset = position;
  llvm::SmallVector<std::string, 4> key;
  if (!parseKey(key))
    return false;
  skipWhitespace();
  if (peek() != '=')
    return fail("expected '=' after the key");
  ++position;
  skipWhitespace();
  std::optional<Value> value;
  if (!parseValue(value))
    return false;
  return assignDotted(table, key, keyOffset, std::move(*value));
}

bool Parser::parseEscape(std::string &out) {
  // At the backslash.
  std::size_t escapeOffset = position;
  ++position;
  char c = peek();
  ++position;
  switch (c) {
  case 'b':
    out += '\b';
    return true;
  case 't':
    out += '\t';
    return true;
  case 'n':
    out += '\n';
    return true;
  case 'f':
    out += '\f';
    return true;
  case 'r':
    out += '\r';
    return true;
  case '"':
    out += '"';
    return true;
  case '\\':
    out += '\\';
    return true;
  case 'u':
  case 'U': {
    std::size_t digits = c == 'u' ? 4 : 8;
    std::uint32_t codePoint = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      char digit = peek();
      if (!isHexDigit(digit))
        return failAt(escapeOffset, llvm::Twine("\\") + llvm::Twine(c) +
                                        " needs " + llvm::Twine(digits) +
                                        " hexadecimal digits");
      codePoint = codePoint * 16 +
                  static_cast<std::uint32_t>(
                      isDigit(digit) ? digit - '0'
                                     : (digit | 0x20) - 'a' + 10);
      ++position;
    }
    if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
      return failAt(escapeOffset, "the escape is not a Unicode scalar value");
    appendUTF8(out, codePoint);
    return true;
  }
  default:
    return failAt(escapeOffset, "unknown escape sequence");
  }
}

bool Parser::parseBasicString(std::string &out) {
  std::size_t start = position;
  ++position;
  while (true) {
    if (atEnd() || atNewline() || peek() == '\r')
      return failAt(start, "unterminated string");
    char c = peek();
    if (c == '"') {
      ++position;
      return true;
    }
    if (c == '\\') {
      if (!parseEscape(out))
        return false;
      continue;
    }
    if (isControl(static_cast<unsigned char>(c)))
      return fail("control character in a string");
    out += c;
    ++position;
  }
}

bool Parser::parseMultilineBasicString(std::string &out) {
  std::size_t start = position;
  position += 3;
  skipNewline();
  while (true) {
    if (atEnd())
      return failAt(start, "unterminated multi-line string");
    if (startsWith("\"\"\"")) {
      std::size_t quotes = 0;
      while (peek(quotes) == '"')
        ++quotes;
      if (quotes > 5)
        return fail("too many quotes at the end of a multi-line string");
      out.append(quotes - 3, '"');
      position += quotes;
      return true;
    }
    if (skipNewline()) {
      out += '\n';
      continue;
    }
    char c = peek();
    if (c == '\\') {
      // A line-ending backslash trims the newline and the whitespace after.
      std::size_t afterBackslash = position + 1;
      std::size_t cursor = afterBackslash;
      while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
        ++cursor;
      llvm::StringRef rest = text.substr(cursor);
      if (rest.starts_with("\n") || rest.starts_with("\r\n")) {
        position = cursor;
        while (true) {
          skipWhitespace();
          if (!skipNewline())
            break;
        }
        continue;
      }
      if (!parseEscape(out))
        return false;
      continue;
    }
    if (isControl(static_cast<unsigned char>(c)))
      return fail(c == '\r' ? "a carriage return must be followed by a line feed"
                            : "control character in a string");
    out += c;
    ++position;
  }
}

bool Parser::parseLiteralString(std::string &out) {
  std::size_t start = position;
  ++position;
  while (true) {
    if (atEnd() || atNewline() || peek() == '\r')
      return failAt(start, "unterminated string");
    char c = peek();
    if (c == '\'') {
      ++position;
      return true;
    }
    if (isControl(static_cast<unsigned char>(c)))
      return fail("control character in a string");
    out += c;
    ++position;
  }
}

bool Parser::parseMultilineLiteralString(std::string &out) {
  std::size_t start = position;
  position += 3;
  skipNewline();
  while (true) {
    if (atEnd())
      return failAt(start, "unterminated multi-line string");
    if (startsWith("'''")) {
      std::size_t quotes = 0;
      while (peek(quotes) == '\'')
        ++quotes;
      if (quotes > 5)
        return fail("too many quotes at the end of a multi-line string");
      out.append(quotes - 3, '\'');
      position += quotes;
      return true;
    }
    if (skipNewline()) {
      out += '\n';
      continue;
    }
    char c = peek();
    if (isControl(static_cast<unsigned char>(c)))
      return fail(c == '\r' ? "a carriage return must be followed by a line feed"
                            : "control character in a string");
    out += c;
    ++position;
  }
}

bool Parser::parseArray(std::optional<Value> &value) {
  ++position;
  Value array = Value::array();
  while (true) {
    if (!skipWhitespaceCommentsAndNewlines())
      return false;
    if (peek() == ']') {
      ++position;
      break;
    }
    std::optional<Value> element;
    if (!parseValue(element))
      return false;
    array.asArray()->push_back(std::move(*element));
    if (!skipWhitespaceCommentsAndNewlines())
      return false;
    if (peek() == ',') {
      ++position;
      continue;
    }
    if (peek() == ']') {
      ++position;
      break;
    }
    return fail("expected ',' or ']' in an array");
  }
  value = std::move(array);
  return true;
}

void Parser::freezeInline(const Table &table) {
  origins[&table] = Origin::Inline;
  for (const Table::Entry &entry : table)
    if (const Table *inner = entry.second.asTable())
      freezeInline(*inner);
}

bool Parser::parseInlineTable(std::optional<Value> &value) {
  ++position;
  Value table = Value::table();
  skipWhitespace();
  if (peek() == '}') {
    ++position;
  } else {
    while (true) {
      skipWhitespace();
      if (!parseKeyValue(*table.asTable()))
        return false;
      skipWhitespace();
      if (peek() == ',') {
        ++position;
        skipWhitespace();
        if (peek() == '}')
          return fail("an inline table cannot end with a trailing comma");
        continue;
      }
      if (peek() == '}') {
        ++position;
        break;
      }
      if (atNewline() || peek() == '#')
        return fail("an inline table must be written on one line");
      return fail("expected ',' or '}' in an inline table");
    }
  }
  freezeInline(*table.asTable());
  value = std::move(table);
  return true;
}

// `digits` matches DIGIT *( DIGIT / "_" DIGIT ) for the digit class `accepts`
// from `cursor`, and leaves `cursor` after the match.
template <typename Accepts>
bool matchDigits(llvm::StringRef token, std::size_t &cursor, Accepts accepts) {
  if (cursor >= token.size() || !accepts(token[cursor]))
    return false;
  ++cursor;
  while (cursor < token.size()) {
    if (accepts(token[cursor])) {
      ++cursor;
    } else if (token[cursor] == '_' && cursor + 1 < token.size() &&
               accepts(token[cursor + 1])) {
      cursor += 2;
    } else {
      break;
    }
  }
  return true;
}

bool Parser::parseNumber(llvm::StringRef token, std::size_t offset,
                         std::optional<Value> &value) {
  auto invalid = [&]() {
    return failAt(offset, llvm::Twine("invalid value '") + token + "'");
  };
  std::size_t cursor = 0;
  bool negative = false;
  if (token[0] == '+' || token[0] == '-') {
    negative = token[0] == '-';
    ++cursor;
  }
  llvm::StringRef unsignedPart = token.substr(cursor);
  if (unsignedPart == "inf" || unsignedPart == "nan") {
    double number = unsignedPart == "inf"
                        ? std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::quiet_NaN();
    value = Value::floating(negative ? -number : number);
    return true;
  }

  // Prefixed integers carry no sign.
  if (unsignedPart.size() > 2 && unsignedPart[0] == '0' &&
      (unsignedPart[1] == 'x' || unsignedPart[1] == 'o' ||
       unsignedPart[1] == 'b')) {
    if (cursor != 0)
      return invalid();
    unsigned radix = unsignedPart[1] == 'x' ? 16 : unsignedPart[1] == 'o' ? 8 : 2;
    auto accepts = [radix](char c) {
      if (radix == 16)
        return isHexDigit(c);
      return c >= '0' && c < static_cast<char>('0' + radix);
    };
    std::size_t digitsAt = 2;
    if (!matchDigits(unsignedPart, digitsAt, accepts) ||
        digitsAt != unsignedPart.size())
      return invalid();
    std::uint64_t number = 0;
    for (char c : unsignedPart.drop_front(2)) {
      if (c == '_')
        continue;
      unsigned digit = isDigit(c) ? static_cast<unsigned>(c - '0')
                                  : static_cast<unsigned>((c | 0x20) - 'a' + 10);
      if (number > (static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) -
                    digit) /
                       radix)
        return failAt(offset, llvm::Twine("integer '") + token +
                                  "' does not fit in 64 signed bits");
      number = number * radix + digit;
    }
    value = Value::integer(static_cast<std::int64_t>(number));
    return true;
  }

  // dec-int, then an optional fraction and exponent for a float.
  std::size_t integerStart = cursor;
  if (!matchDigits(token, cursor, isDigit))
    return invalid();
  if (token[integerStart] == '0' && cursor - integerStart > 1)
    return failAt(offset, llvm::Twine("leading zeros are not allowed in '") + token + "'");
  bool isFloat = false;
  if (cursor < token.size() && token[cursor] == '.') {
    ++cursor;
    if (!matchDigits(token, cursor, isDigit))
      return invalid();
    isFloat = true;
  }
  if (cursor < token.size() && (token[cursor] == 'e' || token[cursor] == 'E')) {
    ++cursor;
    if (cursor < token.size() && (token[cursor] == '+' || token[cursor] == '-'))
      ++cursor;
    if (!matchDigits(token, cursor, isDigit))
      return invalid();
    isFloat = true;
  }
  if (cursor != token.size())
    return invalid();

  llvm::SmallString<32> digits;
  for (char c : token)
    if (c != '_')
      digits += c;
  if (isFloat) {
    llvm::APFloat number(llvm::APFloat::IEEEdouble());
    auto status =
        number.convertFromString(digits, llvm::APFloat::rmNearestTiesToEven);
    if (!status) {
      llvm::consumeError(status.takeError());
      return invalid();
    }
    // Overflow rounds to infinity and underflow towards zero, as IEEE 754
    // binary64 does (and as CPython's tomllib reads them).
    value = Value::floating(number.convertToDouble());
    return true;
  }
  std::uint64_t magnitude = 0;
  std::uint64_t limit =
      negative ? static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()) +
                     1
               : static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max());
  for (char c : llvm::StringRef(digits).drop_front(integerStart)) {
    unsigned digit = static_cast<unsigned>(c - '0');
    if (magnitude > (limit - digit) / 10)
      return failAt(offset,
                    llvm::Twine("integer '") + token + "' does not fit in 64 signed bits");
    magnitude = magnitude * 10 + digit;
  }
  std::int64_t number =
      negative ? static_cast<std::int64_t>(0 - magnitude)
               : static_cast<std::int64_t>(magnitude);
  value = Value::integer(number);
  return true;
}

// `count` decimal digits at `cursor` as a number, or -1.
int fixedDigits(llvm::StringRef token, std::size_t cursor, std::size_t count) {
  if (cursor + count > token.size())
    return -1;
  int number = 0;
  for (std::size_t index = 0; index < count; ++index) {
    char c = token[cursor + index];
    if (!isDigit(c))
      return -1;
    number = number * 10 + (c - '0');
  }
  return number;
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year))
    return 29;
  return days[month - 1];
}

bool Parser::parseDateTime(llvm::StringRef token, std::size_t offset,
                           std::optional<Value> &value) {
  auto invalid = [&]() {
    return failAt(offset, llvm::Twine("invalid date or time '") + token + "'");
  };
  std::size_t cursor = 0;
  bool hasDate = false;
  if (token.size() >= 10 && token[4] == '-') {
    int year = fixedDigits(token, 0, 4);
    int month = token[7] == '-' ? fixedDigits(token, 5, 2) : -1;
    int day = fixedDigits(token, 8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 ||
        day > daysInMonth(year, month))
      return invalid();
    hasDate = true;
    cursor = 10;
    if (cursor == token.size()) {
      value = Value::dateTime(Value::Kind::LocalDate, token.str());
      return true;
    }
    char separator = token[cursor];
    if (separator != 'T' && separator != 't' && separator != ' ')
      return invalid();
    ++cursor;
  }

  // partial-time = 2DIGIT ":" 2DIGIT ":" 2DIGIT [ "." 1*DIGIT ]
  int hour = fixedDigits(token, cursor, 2);
  int minute = cursor + 2 < token.size() && token[cursor + 2] == ':'
                   ? fixedDigits(token, cursor + 3, 2)
                   : -1;
  int second = cursor + 5 < token.size() && token[cursor + 5] == ':'
                   ? fixedDigits(token, cursor + 6, 2)
                   : -1;
  // RFC 3339 admits 60 for a leap second.
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 60)
    return invalid();
  cursor += 8;
  if (cursor < token.size() && token[cursor] == '.') {
    std::size_t fractionStart = ++cursor;
    while (cursor < token.size() && isDigit(token[cursor]))
      ++cursor;
    if (cursor == fractionStart)
      return invalid();
  }
  if (cursor == token.size()) {
    value = Value::dateTime(hasDate ? Value::Kind::LocalDateTime
                                    : Value::Kind::LocalTime,
                            token.str());
    return true;
  }
  if (!hasDate)
    return invalid();
  // time-offset = "Z" / time-numoffset
  if (token[cursor] == 'Z' || token[cursor] == 'z') {
    ++cursor;
  } else if (token[cursor] == '+' || token[cursor] == '-') {
    int offsetHour = fixedDigits(token, cursor + 1, 2);
    int offsetMinute = cursor + 3 < token.size() && token[cursor + 3] == ':'
                           ? fixedDigits(token, cursor + 4, 2)
                           : -1;
    if (offsetHour < 0 || offsetHour > 23 || offsetMinute < 0 ||
        offsetMinute > 59)
      return invalid();
    cursor += 6;
  } else {
    return invalid();
  }
  if (cursor != token.size())
    return invalid();
  value = Value::dateTime(Value::Kind::OffsetDateTime, token.str());
  return true;
}

bool Parser::parseScalarToken(std::optional<Value> &value) {
  std::size_t start = position;
  auto isTokenChar = [](char c) {
    return isBareKeyChar(c) || c == '+' || c == '.' || c == ':';
  };
  while (isTokenChar(peek()))
    ++position;
  // A date and a time may be separated by one space.
  llvm::StringRef soFar = text.slice(start, position);
  if (soFar.size() == 10 && soFar[4] == '-' && soFar[7] == '-' &&
      peek() == ' ' && isDigit(peek(1)) && isDigit(peek(2)) && peek(3) == ':') {
    ++position;
    while (isTokenChar(peek()))
      ++position;
  }
  llvm::StringRef token = text.slice(start, position);
  if (token.empty())
    return failAt(start, "expected a value");
  if (token == "true" || token == "false") {
    value = Value::boolean(token == "true");
    return true;
  }
  bool looksLikeDate = token.size() >= 10 && isDigit(token[0]) &&
                       token[4] == '-' && isDigit(token[1]);
  bool looksLikeTime = token.size() >= 8 && isDigit(token[0]) &&
                       isDigit(token[1]) && token[2] == ':';
  if (looksLikeDate || looksLikeTime)
    return parseDateTime(token, start, value);
  return parseNumber(token, start, value);
}

bool Parser::parseValue(std::optional<Value> &value) {
  if (startsWith("\"\"\"")) {
    std::string text;
    if (!parseMultilineBasicString(text))
      return false;
    value = Value::string(std::move(text));
    return true;
  }
  if (startsWith("'''")) {
    std::string text;
    if (!parseMultilineLiteralString(text))
      return false;
    value = Value::string(std::move(text));
    return true;
  }
  if (peek() == '"' || peek() == '\'') {
    std::string text;
    if (!(peek() == '"' ? parseBasicString(text) : parseLiteralString(text)))
      return false;
    value = Value::string(std::move(text));
    return true;
  }
  if (peek() == '[' || peek() == '{') {
    if (depth == maxDepth)
      return fail("arrays and inline tables nest deeper than " +
                  llvm::Twine(maxDepth) + " levels");
    ++depth;
    bool parsed = peek() == '[' ? parseArray(value) : parseInlineTable(value);
    --depth;
    return parsed;
  }
  return parseScalarToken(value);
}

ParseResult Parser::run() {
  ParseResult result;
  if (!validateEncoding()) {
    result.error = std::move(error);
    return result;
  }
  root = &result.table;
  origins[root] = Origin::Header;
  Table *current = root;
  while (!error) {
    skipWhitespace();
    if (atEnd())
      break;
    if (peek() == '#' || atNewline()) {
      if (!expectEndOfLine())
        break;
      continue;
    }
    if (peek() == '[') {
      if (!parseHeader(current) || !expectEndOfLine())
        break;
      continue;
    }
    if (!parseKeyValue(*current) || !expectEndOfLine())
      break;
  }
  if (error) {
    result.table = Table();
    result.error = std::move(error);
  }
  return result;
}

} // namespace

ParseResult Table::parse(llvm::StringRef document, unsigned maxDepth) {
  return Parser(document, maxDepth).run();
}

} // namespace lython::toml
