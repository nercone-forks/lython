#pragma once

// A TOML 1.0.0 reader (https://toml.io/en/v1.0.0).
//
// The whole document is validated: a file the specification calls invalid is
// refused with the line and column of the first violation, never read
// partially. Date-time values are validated and kept as their source spelling,
// because nothing here computes with them and a calendar type would be a
// second, unrequested library.

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lython::toml {

class Table;
class Value;

using Array = std::vector<Value>;

class Value {
public:
  enum class Kind {
    String,
    Integer,
    Float,
    Boolean,
    OffsetDateTime,
    LocalDateTime,
    LocalDate,
    LocalTime,
    Array,
    Table,
  };

  static Value string(std::string text);
  static Value integer(std::int64_t number);
  static Value floating(double number);
  static Value boolean(bool truth);
  // `kind` is one of the four date-time kinds; `spelling` is the value as
  // written in the document.
  static Value dateTime(Kind kind, std::string spelling);
  static Value array();
  static Value table();

  // Out of line: a Table is incomplete here.
  Value(Value &&) noexcept;
  Value &operator=(Value &&) noexcept;
  Value(const Value &) = delete;
  Value &operator=(const Value &) = delete;
  ~Value();

  Kind kind() const { return valueKind; }
  bool isDateTime() const;

  // Null (or empty) when the value is of another kind.
  const std::string *asString() const;
  std::optional<std::int64_t> asInteger() const;
  std::optional<double> asFloat() const;
  std::optional<bool> asBoolean() const;
  // The source spelling of a date-time value.
  const std::string *asDateTime() const;
  const Array *asArray() const;
  Array *asArray();
  const Table *asTable() const;
  Table *asTable();

  // "string", "integer", "float", "boolean", "offset date-time",
  // "local date-time", "local date", "local time", "array" or "table".
  static llvm::StringRef kindName(Kind kind);

private:
  Value(Kind kind) : valueKind(kind) {}

  Kind valueKind;
  // Arrays and tables live on the heap: their addresses stay put while the
  // table that holds them grows, and the scalars stay small.
  std::variant<std::monostate, std::string, std::int64_t, double, bool,
               std::unique_ptr<Array>, std::unique_ptr<Table>>
      storage;
};

struct ParseError {
  // 1-based; the column counts Unicode scalar values.
  unsigned line = 0;
  unsigned column = 0;
  std::string message;
};

struct ParseResult;

// Keys keep the order the document defines them in.
class Table {
public:
  using Entry = std::pair<std::string, Value>;

  Table() = default;
  Table(Table &&) = default;
  Table &operator=(Table &&) = default;

  // Parses a whole TOML document, which is a table. Arrays and inline tables
  // nested deeper than `maxDepth` are refused (the specification sets no
  // limit; a reader has to, or a hostile file exhausts its stack).
  static constexpr unsigned defaultMaxDepth = 256;
  static ParseResult parse(llvm::StringRef document,
                           unsigned maxDepth = defaultMaxDepth);

  const Value *find(llvm::StringRef key) const;
  Value *find(llvm::StringRef key);
  bool contains(llvm::StringRef key) const { return find(key) != nullptr; }
  // The key must not be present yet. Returns the stored value.
  Value &insert(std::string key, Value value);

  // A value reached through a dotted path of keys (`{"project", "name"}`);
  // null when a key is missing or an intermediate value is not a table.
  const Value *lookup(llvm::ArrayRef<llvm::StringRef> path) const;

  std::size_t size() const { return entries.size(); }
  bool empty() const { return entries.empty(); }
  std::vector<Entry>::const_iterator begin() const { return entries.begin(); }
  std::vector<Entry>::const_iterator end() const { return entries.end(); }

private:
  std::vector<Entry> entries;
  llvm::StringMap<std::size_t> index;
};

struct ParseResult {
  Table table;
  std::optional<ParseError> error;

  bool ok() const { return !error.has_value(); }
};

} // namespace lython::toml
