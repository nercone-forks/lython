#include "Project.h"

#include "TOML.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace lython::driver {

namespace {

bool isIdentifierStart(unsigned char c) {
  // Non-ASCII bytes are left to the Python parser, which reads the
  // synthesized source and refuses what is not an identifier. None of them
  // is a quote, a newline or an operator, so nothing can be injected.
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
         c >= 0x80;
}

bool isIdentifierChar(unsigned char c) {
  return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

// Python's hard keywords, which no module or attribute can be named. The soft
// ones (`match`, `case`, `type`, `_`) are identifiers.
bool isKeyword(llvm::StringRef word) {
  static const char *const keywords[] = {
      "False",  "None",     "True",    "and",    "as",       "assert",
      "async",  "await",    "break",   "class",  "continue", "def",
      "del",    "elif",     "else",    "except", "finally",  "for",
      "from",   "global",   "if",      "import", "in",       "is",
      "lambda", "nonlocal", "not",     "or",     "pass",     "raise",
      "return", "try",      "while",   "with",   "yield"};
  for (const char *keyword : keywords)
    if (word == keyword)
      return true;
  return false;
}

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

// `a.b.c` as its parts, each a Python identifier; false when it is not one.
bool splitDottedName(llvm::StringRef text,
                     std::vector<std::string> &parts) {
  if (text.empty())
    return false;
  llvm::SmallVector<llvm::StringRef, 4> pieces;
  text.split(pieces, '.');
  for (llvm::StringRef piece : pieces) {
    if (piece.empty() || !isIdentifierStart(piece.front()) || isKeyword(piece))
      return false;
    for (char c : piece)
      if (!isIdentifierChar(static_cast<unsigned char>(c)))
        return false;
    parts.push_back(piece.str());
  }
  return true;
}

std::string joinDotted(const std::vector<std::string> &parts) {
  std::string joined;
  for (const std::string &part : parts) {
    if (!joined.empty())
      joined += '.';
    joined += part;
  }
  return joined;
}

// ⛔ Not `exists` alone, for the reason the import resolver gives
// (Frontend.cpp, existsWithExactCase): on a case-insensitive file system
// `Pkg/` exists when `pkg/` does, and CPython's finder compares the case.
bool existsWithExactCase(llvm::StringRef path) {
  if (!llvm::sys::fs::exists(path))
    return false;
  llvm::SmallString<256> real;
  if (llvm::sys::fs::real_path(path, real))
    return true;
  return llvm::sys::path::filename(real) == llvm::sys::path::filename(path);
}

} // namespace

std::optional<EntryPoint> EntryPoint::parse(llvm::StringRef text,
                                            std::string &error) {
  // modulename \s* (":" \s* objectname \s*)? ("[" extras "]" \s*)?
  llvm::StringRef rest = text.ltrim(" \t\n\r\f\v");
  std::size_t moduleEnd = 0;
  while (moduleEnd < rest.size() &&
         (isIdentifierChar(static_cast<unsigned char>(rest[moduleEnd])) ||
          rest[moduleEnd] == '.'))
    ++moduleEnd;
  EntryPoint entry;
  if (!splitDottedName(rest.take_front(moduleEnd), entry.attributes)) {
    error = "'" + text.str() + "' does not start with a module name";
    return std::nullopt;
  }
  entry.module = joinDotted(entry.attributes);
  entry.attributes.clear();
  rest = rest.drop_front(moduleEnd);
  while (!rest.empty() && isSpace(rest.front()))
    rest = rest.drop_front();
  if (rest.consume_front(":")) {
    while (!rest.empty() && isSpace(rest.front()))
      rest = rest.drop_front();
    std::size_t objectEnd = 0;
    while (objectEnd < rest.size() &&
           (isIdentifierChar(static_cast<unsigned char>(rest[objectEnd])) ||
            rest[objectEnd] == '.'))
      ++objectEnd;
    if (!splitDottedName(rest.take_front(objectEnd), entry.attributes)) {
      error = "'" + text.str() + "' names no object after ':'";
      return std::nullopt;
    }
    rest = rest.drop_front(objectEnd);
    while (!rest.empty() && isSpace(rest.front()))
      rest = rest.drop_front();
  }
  if (rest.starts_with("[")) {
    std::size_t close = rest.rfind(']');
    if (close == llvm::StringRef::npos) {
      error = "'" + text.str() + "' has an unclosed extras list";
      return std::nullopt;
    }
    rest = rest.drop_front(close + 1);
    while (!rest.empty() && isSpace(rest.front()))
      rest = rest.drop_front();
  }
  if (!rest.empty()) {
    error = "'" + text.str() + "' is not an object reference "
            "(expected 'module:object')";
    return std::nullopt;
  }
  return entry;
}

std::string EntryPoint::spelling() const {
  if (attributes.empty())
    return module;
  return module + ":" + joinDotted(attributes);
}

bool Project::isProjectDirectory(llvm::StringRef directory) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, "pyproject.toml");
  return llvm::sys::fs::is_directory(directory) &&
         llvm::sys::fs::is_regular_file(path);
}

std::optional<Project> Project::load(llvm::StringRef directory,
                                     llvm::raw_ostream &diag) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, "pyproject.toml");
  auto file = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                          /*RequiresNullTerminator=*/false);
  if (!file) {
    diag << "error: could not open project file '" << path << "'\n";
    return std::nullopt;
  }
  toml::ParseResult parsed = toml::Table::parse((*file)->getBuffer());
  if (!parsed.ok()) {
    diag << path << ':' << parsed.error->line << ':' << parsed.error->column
         << ": error: " << parsed.error->message << "\n";
    return std::nullopt;
  }

  Project project;
  // Absolute and without `.` or `..`: every path of the program is derived
  // from it, and the import resolver tells `__main__` apart from a module by
  // comparing such paths, which it normalizes the same way.
  llvm::SmallString<256> root(directory);
  if (std::error_code error = llvm::sys::fs::make_absolute(root)) {
    diag << "error: cannot resolve '" << directory << "': " << error.message()
         << "\n";
    return std::nullopt;
  }
  llvm::sys::path::remove_dots(root, /*remove_dot_dot=*/true);
  project.rootDirectory = root.str().str();
  llvm::SmallString<256> sourceDirectory(root);
  llvm::sys::path::append(sourceDirectory, "src");
  project.importDirectory = llvm::sys::fs::is_directory(sourceDirectory)
                                ? sourceDirectory.str().str()
                                : project.rootDirectory;

  const toml::Value *table = parsed.table.find("project");
  if (!table)
    return project;
  const toml::Table *metadata = table->asTable();
  if (!metadata) {
    diag << path << ": error: 'project' must be a table\n";
    return std::nullopt;
  }
  if (const toml::Value *name = metadata->find("name")) {
    if (!name->asString()) {
      diag << path << ": error: 'project.name' must be a string\n";
      return std::nullopt;
    }
    project.projectName = *name->asString();
  }
  for (llvm::StringRef section : {"scripts", "gui-scripts"}) {
    const toml::Value *value = metadata->find(section);
    if (!value)
      continue;
    const toml::Table *scripts = value->asTable();
    if (!scripts) {
      diag << path << ": error: 'project." << section
           << "' must be a table\n";
      return std::nullopt;
    }
    for (const toml::Table::Entry &script : *scripts) {
      const std::string *reference = script.second.asString();
      if (!reference) {
        diag << path << ": error: 'project." << section << "." << script.first
             << "' must be a string\n";
        return std::nullopt;
      }
      std::string error;
      std::optional<EntryPoint> entry = EntryPoint::parse(*reference, error);
      if (!entry) {
        diag << path << ": error: 'project." << section << "." << script.first
             << "': " << error << "\n";
        return std::nullopt;
      }
      if (entry->attributes.empty()) {
        diag << path << ": error: 'project." << section << "." << script.first
             << "' must name a callable ('module:function'), not only the "
                "module '"
             << entry->module << "'\n";
        return std::nullopt;
      }
      if (project.script(script.first)) {
        diag << path << ": error: script '" << script.first
             << "' is defined in both 'project.scripts' and "
                "'project.gui-scripts'\n";
        return std::nullopt;
      }
      project.scriptEntries.emplace_back(script.first, std::move(*entry));
    }
  }
  return project;
}

const EntryPoint *Project::script(llvm::StringRef name) const {
  for (const auto &entry : scriptEntries)
    if (entry.first == name)
      return &entry.second;
  return nullptr;
}

std::optional<MainModule>
Project::mainModule(llvm::StringRef moduleName, llvm::raw_ostream &diag) const {
  std::vector<std::string> parts;
  if (!splitDottedName(moduleName, parts)) {
    diag << "error: '" << moduleName << "' is not a module name\n";
    return std::nullopt;
  }
  llvm::SmallString<256> path(importDirectory);
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    llvm::sys::path::append(path, parts[index]);
    llvm::SmallString<256> init(path);
    llvm::sys::path::append(init, "__init__.py");
    if (!existsWithExactCase(path) || !existsWithExactCase(init)) {
      diag << "error: no package named '"
           << joinDotted(std::vector<std::string>(
                  parts.begin(), parts.begin() + index + 1))
           << "' in " << importDirectory << "\n";
      return std::nullopt;
    }
  }
  llvm::sys::path::append(path, parts.back());
  llvm::SmallString<256> init(path);
  llvm::sys::path::append(init, "__init__.py");
  if (existsWithExactCase(path) && existsWithExactCase(init)) {
    llvm::SmallString<256> main(path);
    llvm::sys::path::append(main, "__main__.py");
    if (!existsWithExactCase(main)) {
      diag << "error: no module named '" << moduleName
           << ".__main__'; '" << moduleName
           << "' is a package and cannot be directly executed\n";
      return std::nullopt;
    }
    return MainModule{main.str().str(), moduleName.str()};
  }
  llvm::SmallString<256> source(path);
  source += ".py";
  if (!existsWithExactCase(source)) {
    diag << "error: no module named '" << moduleName << "' in "
         << importDirectory << "\n";
    return std::nullopt;
  }
  parts.pop_back();
  return MainModule{source.str().str(), joinDotted(parts)};
}

std::string Project::scriptSource(const EntryPoint &entry) {
  // The name the object is bound to; `sys` is taken by the exit.
  std::string bound = entry.attributes.front();
  std::string import = "from " + entry.module + " import " + bound;
  if (bound == "sys") {
    bound = "__lython_entry_point";
    import += " as " + bound;
  }
  std::string call = bound;
  for (std::size_t index = 1; index < entry.attributes.size(); ++index)
    call += "." + entry.attributes[index];
  return "import sys\n" + import + "\nsys.exit(" + call + "())\n";
}

std::string Project::scriptPath(llvm::StringRef name) const {
  llvm::SmallString<256> path(rootDirectory);
  llvm::sys::path::append(path, name);
  return path.str().str();
}

std::string Project::scriptSourcePath(llvm::StringRef name) const {
  return scriptPath(name) + "-script.py";
}

} // namespace lython::driver
