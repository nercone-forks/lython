// What this pins: reading a project directory -- its pyproject.toml through
// the TOML 1.0.0 reader, the entry points its `[project.scripts]` name, and
// the module `python -m` would run -- and that the modules of a package import
// each other by their absolute names, from the import root, as CPython's
// sys.path has them do.
//
// The TOML expectations are the specification's (toml.io/en/v1.0.0), not the
// reader's: each valid and invalid document below was also given to CPython
// 3.14's tomllib, which agrees on all of them except where noted.

#include "Driver.h"
#include "Project.h"
#include "TOML.h"

#include "mlir/IR/MLIRContext.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using lython::toml::Table;
using lython::toml::Value;

Table parseValid(llvm::StringRef document) {
  lython::toml::ParseResult result = Table::parse(document);
  EXPECT_TRUE(result.ok()) << document.str() << "\n-> "
                           << (result.error ? result.error->message : "");
  return std::move(result.table);
}

const Value *at(const Table &table,
                std::initializer_list<llvm::StringRef> path) {
  return table.lookup(llvm::ArrayRef<llvm::StringRef>(path.begin(), path.size()));
}

// --- TOML ---------------------------------------------------------------------

TEST(TOMLTest, ReadsEveryScalarKind) {
  Table table = parseValid(
      "s = \"basic \\u00e9\\U0001F600\\t\\\"\"\n"
      "l = 'C:\\Users\\nodejs'\n"
      "i = +17\n"
      "n = -9223372036854775808\n"
      "m = 9223372036854775807\n"
      "h = 0xDEAD_beef\n"
      "o = 0o755\n"
      "b = 0b1101\n"
      "f = 6.626e-34\n"
      "g = 9_224_617.445_991\n"
      "pinf = +inf\n"
      "ninf = -inf\n"
      "qnan = nan\n"
      "huge = 1e400\n"
      "t = true\n"
      "u = false\n");
  EXPECT_EQ(*at(table, {"s"})->asString(), "basic \xc3\xa9\xf0\x9f\x98\x80\t\"");
  EXPECT_EQ(*at(table, {"l"})->asString(), "C:\\Users\\nodejs");
  EXPECT_EQ(at(table, {"i"})->asInteger(), 17);
  EXPECT_EQ(at(table, {"n"})->asInteger(), INT64_MIN);
  EXPECT_EQ(at(table, {"m"})->asInteger(), INT64_MAX);
  EXPECT_EQ(at(table, {"h"})->asInteger(), 0xDEADBEEF);
  EXPECT_EQ(at(table, {"o"})->asInteger(), 0755);
  EXPECT_EQ(at(table, {"b"})->asInteger(), 13);
  EXPECT_DOUBLE_EQ(*at(table, {"f"})->asFloat(), 6.626e-34);
  EXPECT_DOUBLE_EQ(*at(table, {"g"})->asFloat(), 9224617.445991);
  EXPECT_TRUE(std::isinf(*at(table, {"pinf"})->asFloat()));
  EXPECT_GT(*at(table, {"pinf"})->asFloat(), 0);
  EXPECT_LT(*at(table, {"ninf"})->asFloat(), 0);
  EXPECT_TRUE(std::isnan(*at(table, {"qnan"})->asFloat()));
  // IEEE 754 binary64 overflows to infinity (tomllib reads it the same way).
  EXPECT_TRUE(std::isinf(*at(table, {"huge"})->asFloat()));
  EXPECT_EQ(at(table, {"t"})->asBoolean(), true);
  EXPECT_EQ(at(table, {"u"})->asBoolean(), false);
  // A value answers only for its own kind.
  EXPECT_EQ(at(table, {"i"})->asString(), nullptr);
  EXPECT_FALSE(at(table, {"s"})->asInteger());
}

TEST(TOMLTest, ReadsDateTimesAsTheirKindAndSpelling) {
  Table table = parseValid("odt = 1979-05-27T07:32:00.999999-07:00\n"
                           "space = 1979-05-27 07:32:00Z\n"
                           "lower = 1979-05-27t07:32:00z\n"
                           "ldt = 1979-05-27T07:32:00\n"
                           "ld = 2000-02-29\n"
                           "lt = 07:32:00.5\n");
  EXPECT_EQ(at(table, {"odt"})->kind(), Value::Kind::OffsetDateTime);
  EXPECT_EQ(*at(table, {"odt"})->asDateTime(),
            "1979-05-27T07:32:00.999999-07:00");
  EXPECT_EQ(at(table, {"space"})->kind(), Value::Kind::OffsetDateTime);
  EXPECT_EQ(*at(table, {"space"})->asDateTime(), "1979-05-27 07:32:00Z");
  EXPECT_EQ(at(table, {"lower"})->kind(), Value::Kind::OffsetDateTime);
  EXPECT_EQ(at(table, {"ldt"})->kind(), Value::Kind::LocalDateTime);
  EXPECT_EQ(at(table, {"ld"})->kind(), Value::Kind::LocalDate);
  EXPECT_EQ(at(table, {"lt"})->kind(), Value::Kind::LocalTime);
}

TEST(TOMLTest, ReadsMultilineStringsAsTheSpecificationTrimsThem) {
  Table table = parseValid("a = \"\"\"\nRoses\nViolets\"\"\"\n"
                           "b = \"\"\"\\\n   The quick \\\n\n   fox.\"\"\"\n"
                           "c = '''\nx''''\n"
                           "d = \"\"\"Here are two quotes: \"\". \"\"\"\"\"\n"
                           "e = '''\\n is not an escape'''\n"
                           "f = \"\"\"a\r\nb\"\"\"\n");
  EXPECT_EQ(*at(table, {"a"})->asString(), "Roses\nViolets");
  EXPECT_EQ(*at(table, {"b"})->asString(), "The quick fox.");
  EXPECT_EQ(*at(table, {"c"})->asString(), "x'");
  EXPECT_EQ(*at(table, {"d"})->asString(), "Here are two quotes: \"\". \"\"");
  EXPECT_EQ(*at(table, {"e"})->asString(), "\\n is not an escape");
  // A CRLF inside a multi-line string is a newline.
  EXPECT_EQ(*at(table, {"f"})->asString(), "a\nb");
}

TEST(TOMLTest, ReadsKeysTablesAndArraysInDocumentOrder) {
  Table table = parseValid(
      "\"\" = 1\n"
      "'q' = 2\n"
      "\"a.b\" = 3\n"
      " site . \"google.com\" = true\n"
      "arr = [ 1, 'x', [2], {b = 3}, ]\n"
      "multi = [\n  1, # one\n  2\n]\n"
      "\n"
      "[dog.\"tater.man\"]\n"
      "type.name = \"pug\"\n"
      "\n"
      "[[products]]\n"
      "name = \"Hammer\"\n"
      "[[products]]\n"
      "[products.dim]\n"
      "w = 2\n");
  std::vector<std::string> keys;
  for (const Table::Entry &entry : table)
    keys.push_back(entry.first);
  EXPECT_EQ(keys, (std::vector<std::string>{"", "q", "a.b", "site", "arr",
                                            "multi", "dog", "products"}));
  EXPECT_EQ(at(table, {"site", "google.com"})->asBoolean(), true);
  const lython::toml::Array &arr = *at(table, {"arr"})->asArray();
  ASSERT_EQ(arr.size(), 4u);
  EXPECT_EQ(arr[0].asInteger(), 1);
  EXPECT_EQ(*arr[1].asString(), "x");
  EXPECT_EQ((*arr[2].asArray())[0].asInteger(), 2);
  EXPECT_EQ(arr[3].asTable()->find("b")->asInteger(), 3);
  EXPECT_EQ(at(table, {"multi"})->asArray()->size(), 2u);
  EXPECT_EQ(*at(table, {"dog", "tater.man", "type", "name"})->asString(), "pug");
  const lython::toml::Array &products = *at(table, {"products"})->asArray();
  ASSERT_EQ(products.size(), 2u);
  EXPECT_EQ(*products[0].asTable()->find("name")->asString(), "Hammer");
  EXPECT_TRUE(products[1].asTable()->find("name") == nullptr);
  EXPECT_EQ(products[1].asTable()->lookup({"dim", "w"})->asInteger(), 2);
}

TEST(TOMLTest, AllowsWhatTheSpecificationAllowsAboutDefiningTables) {
  // A super-table may be defined after its sub-table.
  Table later = parseValid("[a.b.c]\nz = 9\n[a]\ny = 1\n");
  EXPECT_EQ(at(later, {"a", "y"})->asInteger(), 1);
  // A header may define a sub-table of a table dotted keys created.
  Table dotted = parseValid("[fruit]\napple.color = 'red'\n"
                            "apple.taste.sweet = true\n"
                            "[fruit.apple.texture]\nsmooth = true\n");
  EXPECT_EQ(at(dotted, {"fruit", "apple", "texture", "smooth"})->asBoolean(),
            true);
  // CRLF line endings.
  Table crlf = parseValid("a = 1\r\nb = 2\r\n");
  EXPECT_EQ(at(crlf, {"b"})->asInteger(), 2);
  EXPECT_TRUE(parseValid("").empty());
}

TEST(TOMLTest, RefusesEveryInvalidDocumentWithItsPosition) {
  const char *invalid[] = {
      // Defining a key or a table twice.
      "a = 1\na = 2\n",
      "[a]\n[a]\n",
      "[fruit]\napple.color = 'red'\n[fruit.apple]\n",
      "[a.b.c]\nz = 9\n[a]\nb.c.t = 9\n",
      "a.b = 1\na.b.c = 2\n",
      "x = 1\n[x.y]\n",
      // Inline tables are complete as written; static arrays are not table
      // arrays.
      "a = {b = 1}\n[a]\n",
      "a = {b = 1}\na.c = 2\n",
      "a = [1]\n[[a]]\n",
      "[[a]]\n[a]\n",
      "[a]\n[[a]]\n",
      "a = {b = 1,}\n",
      "a = {b = 1\n}\n",
      // Numbers.
      "a = 01\n",
      "a = 1__0\n",
      "a = _1\n",
      "a = 1_\n",
      "a = 1.\n",
      "a = .5\n",
      "a = 1e\n",
      "a = +0x1\n",
      "a = 0x\n",
      "a = nan1\n",
      // Not representable in 64 signed bits. tomllib reads these, because a
      // Python int has no width; the specification requires an error when an
      // integer cannot be represented losslessly.
      "a = 9223372036854775808\n",
      "a = -9223372036854775809\n",
      "a = 0x8000000000000000\n",
      // Strings.
      "a = \"\\x41\"\n",
      "a = \"\\uD800\"\n",
      "a = \"abc\n",
      "a = '''abc''''''\n",
      "a = \"\"\"x\\ y\"\"\"\n",
      "\"\"\"k\"\"\" = 1\n",
      "a = \"\x01\"\n",
      // Date-times.
      "a = 1979-02-29\n",
      "a = 1979-13-01\n",
      "a = 07:32\n",
      "a = 1979-05-27T25:00:00\n",
      "a = 1979-05-27 07:32\n",
      "a = 1979-05-27T07:32:00+25:00\n",
      // Everything else.
      "a = TRUE\n",
      "a = truex\n",
      "= 1\n",
      "a\n",
      "a = 1 b = 2\n",
      "# \x01\n",
      "a = 1\r\n\rb = 2\n",
      "[a] b = 1\n",
      "[ [a] ]\n",
      "a = [1 2]\n",
      "a = [1,,2]\n",
      "a = \"\xff\"\n",
  };
  for (const char *document : invalid) {
    lython::toml::ParseResult result = Table::parse(document);
    EXPECT_FALSE(result.ok()) << document;
    if (result.error) {
      EXPECT_GE(result.error->line, 1u) << document;
      EXPECT_GE(result.error->column, 1u) << document;
      EXPECT_FALSE(result.error->message.empty()) << document;
    }
    // Nothing of a refused document is handed out.
    EXPECT_TRUE(result.table.empty()) << document;
  }
}

TEST(TOMLTest, RefusesNestingDeeperThanItsLimit) {
  EXPECT_TRUE(Table::parse("a = [[{b = [1]}]]\n", /*maxDepth=*/4).ok());
  lython::toml::ParseResult refused =
      Table::parse("a = [[{b = [1]}]]\n", /*maxDepth=*/3);
  ASSERT_FALSE(refused.ok());
  EXPECT_NE(refused.error->message.find("deeper than 3"), std::string::npos)
      << refused.error->message;
  // A hostile file is refused with a diagnostic, not by exhausting the stack.
  std::string deep = "a = " + std::string(200000, '[') +
                     std::string(200000, ']') + "\n";
  EXPECT_FALSE(Table::parse(deep).ok());
}

TEST(TOMLTest, NamesTheLineAndColumnOfTheViolation) {
  lython::toml::ParseResult result =
      Table::parse("ok = 1\n\xc3\xa9 = 2\n");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->line, 2u);
  EXPECT_EQ(result.error->column, 1u);

  result = Table::parse("[t]\nkey = \"\xc3\xa9\" x\n");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->line, 2u);
  // Columns count characters, not bytes: `key = "é" ` is 10 of them.
  EXPECT_EQ(result.error->column, 11u);
}

// --- Entry points -----------------------------------------------------------

TEST(EntryPointTest, ReadsAnObjectReference) {
  std::string error;
  std::optional<lython::driver::EntryPoint> entry =
      lython::driver::EntryPoint::parse("pkg.cli:App.main", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_EQ(entry->module, "pkg.cli");
  EXPECT_EQ(entry->attributes, (std::vector<std::string>{"App", "main"}));
  EXPECT_EQ(entry->spelling(), "pkg.cli:App.main");

  // Whitespace around the colon and deprecated extras are accepted; the
  // extras are ignored.
  entry = lython::driver::EntryPoint::parse("  pkg : run  [extra1, x] ", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_EQ(entry->module, "pkg");
  EXPECT_EQ(entry->attributes, (std::vector<std::string>{"run"}));

  entry = lython::driver::EntryPoint::parse("pkg.mod", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_TRUE(entry->attributes.empty());

  // Soft keywords are identifiers.
  entry = lython::driver::EntryPoint::parse("match.type:case", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_EQ(entry->module, "match.type");
}

TEST(EntryPointTest, RefusesWhatIsNotAnObjectReference) {
  for (const char *text :
       {"", ":main", "pkg:", "pkg:.main", "pkg..cli:main", "1pkg:main",
        // Hard keywords name nothing importable.
        "class:run", "pkg.import:run", "pkg:def", "pkg:App.None",
        "pkg:main()", "pkg:main; import os", "pkg:main [x", "pkg-cli:main"}) {
    std::string error;
    EXPECT_FALSE(lython::driver::EntryPoint::parse(text, error)) << text;
    EXPECT_FALSE(error.empty()) << text;
  }
}

// --- Project ----------------------------------------------------------------

class ScratchDirectory {
public:
  ScratchDirectory() {
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("lython-project", path));
  }
  ~ScratchDirectory() { llvm::sys::fs::remove_directories(path); }

  void write(llvm::StringRef relative, llvm::StringRef contents) const {
    llvm::SmallString<256> file(path);
    llvm::sys::path::append(file, relative);
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(file));
    std::error_code error;
    llvm::raw_fd_ostream out(file, error);
    EXPECT_FALSE(error) << file.str().str();
    out << contents;
  }
  std::string join(llvm::StringRef relative) const {
    llvm::SmallString<256> file(path);
    llvm::sys::path::append(file, relative);
    return file.str().str();
  }

  llvm::SmallString<128> path;
};

TEST(ProjectTest, ASrcLayoutImportsFromSrc) {
  ScratchDirectory dir;
  dir.write("pyproject.toml", "[project]\nname = \"demo\"\n\n"
                              "[project.scripts]\n"
                              "demo = \"demo.cli:main\"\n\n"
                              "[project.gui-scripts]\n"
                              "demo-gui = \"demo.gui:App.run\"\n");
  dir.write("src/demo/__init__.py", "");
  std::string diagnostics;
  llvm::raw_string_ostream diag(diagnostics);
  ASSERT_TRUE(lython::driver::Project::isProjectDirectory(dir.path));
  std::optional<lython::driver::Project> project =
      lython::driver::Project::load(dir.path, diag);
  ASSERT_TRUE(project) << diagnostics;
  EXPECT_EQ(project->root(), dir.path.str());
  EXPECT_EQ(project->importRoot(), dir.join("src"));
  EXPECT_EQ(project->name(), "demo");
  ASSERT_EQ(project->scripts().size(), 2u);
  EXPECT_EQ(project->scripts()[0].first, "demo");
  EXPECT_EQ(project->scripts()[1].first, "demo-gui");
  ASSERT_TRUE(project->script("demo-gui"));
  EXPECT_EQ(project->script("demo-gui")->spelling(), "demo.gui:App.run");
  EXPECT_EQ(project->script("missing"), nullptr);
  EXPECT_EQ(project->scriptPath("demo"), dir.join("demo"));
  // Compiled under a name that ends in `.py`, which traceback frames need,
  // and that no importable module can have.
  EXPECT_EQ(project->scriptSourcePath("demo"), dir.join("demo-script.py"));
}

// Every path is absolute and normalized however the directory was spelled:
// the import resolver recognizes `__main__` among the modules by its path.
TEST(ProjectTest, AnswersNormalizedPathsHoweverTheDirectoryIsSpelled) {
  ScratchDirectory dir;
  dir.write("pyproject.toml", "");
  dir.write("src/pkg/__init__.py", "");
  dir.write("src/pkg/mod.py", "");
  std::string diagnostics;
  llvm::raw_string_ostream diag(diagnostics);
  std::optional<lython::driver::Project> project =
      lython::driver::Project::load(dir.join("src/./pkg/../.."), diag);
  ASSERT_TRUE(project) << diagnostics;
  llvm::SmallString<256> expected(dir.path);
  llvm::sys::fs::make_absolute(expected);
  llvm::sys::path::remove_dots(expected, /*remove_dot_dot=*/true);
  EXPECT_EQ(project->root(), expected.str());
  auto main = project->mainModule("pkg.mod", diag);
  ASSERT_TRUE(main) << diagnostics;
  llvm::sys::path::append(expected, "src", "pkg", "mod.py");
  EXPECT_EQ(main->sourcePath, expected.str());
}

TEST(ProjectTest, AFlatLayoutImportsFromTheRoot) {
  ScratchDirectory dir;
  dir.write("pyproject.toml", "[tool.other]\nx = 1\n");
  std::string diagnostics;
  llvm::raw_string_ostream diag(diagnostics);
  std::optional<lython::driver::Project> project =
      lython::driver::Project::load(dir.path, diag);
  ASSERT_TRUE(project) << diagnostics;
  EXPECT_EQ(project->importRoot(), dir.path.str());
  // A file with no `[project]` table is still a project, with no scripts.
  EXPECT_TRUE(project->name().empty());
  EXPECT_TRUE(project->scripts().empty());
}

TEST(ProjectTest, RefusesAProjectFileThatDoesNotDescribeAProject) {
  const std::pair<const char *, const char *> cases[] = {
      {"[project\n", "pyproject.toml:1:9: error:"},
      {"project = 1\n", "'project' must be a table"},
      {"[project]\nname = 1\n", "'project.name' must be a string"},
      {"[project]\nscripts = 1\n", "'project.scripts' must be a table"},
      {"[project.scripts]\nx = 1\n", "'project.scripts.x' must be a string"},
      {"[project.scripts]\nx = \"pkg:\"\n", "names no object after ':'"},
      {"[project.scripts]\nx = \"pkg\"\n", "must name a callable"},
      {"[project.scripts]\nx = \"a:b\"\n[project.gui-scripts]\nx = \"a:c\"\n",
       "defined in both"},
  };
  for (const auto &[contents, expected] : cases) {
    ScratchDirectory dir;
    dir.write("pyproject.toml", contents);
    std::string diagnostics;
    llvm::raw_string_ostream diag(diagnostics);
    EXPECT_FALSE(lython::driver::Project::load(dir.path, diag)) << contents;
    EXPECT_NE(diagnostics.find(expected), std::string::npos)
        << contents << "\n-> " << diagnostics;
  }
  ScratchDirectory empty;
  EXPECT_FALSE(lython::driver::Project::isProjectDirectory(empty.path));
}

TEST(ProjectTest, FindsTheModuleThatPythonDashMRuns) {
  ScratchDirectory dir;
  dir.write("pyproject.toml", "");
  dir.write("src/pkg/__init__.py", "");
  dir.write("src/pkg/__main__.py", "");
  dir.write("src/pkg/tool.py", "");
  dir.write("src/pkg/sub/__init__.py", "");
  dir.write("src/pkg/nsless/mod.py", "");
  dir.write("src/single.py", "");
  std::string diagnostics;
  llvm::raw_string_ostream diag(diagnostics);
  std::optional<lython::driver::Project> project =
      lython::driver::Project::load(dir.path, diag);
  ASSERT_TRUE(project) << diagnostics;

  auto main = project->mainModule("pkg", diag);
  ASSERT_TRUE(main) << diagnostics;
  EXPECT_EQ(main->sourcePath, dir.join("src/pkg/__main__.py"));
  EXPECT_EQ(main->package, "pkg");

  main = project->mainModule("pkg.tool", diag);
  ASSERT_TRUE(main) << diagnostics;
  EXPECT_EQ(main->sourcePath, dir.join("src/pkg/tool.py"));
  EXPECT_EQ(main->package, "pkg");

  main = project->mainModule("single", diag);
  ASSERT_TRUE(main) << diagnostics;
  EXPECT_EQ(main->sourcePath, dir.join("src/single.py"));
  EXPECT_EQ(main->package, "");

  for (const auto &[name, expected] :
       std::vector<std::pair<const char *, const char *>>{
           {"missing", "no module named 'missing'"},
           {"pkg.sub", "'pkg.sub' is a package and cannot be directly "
                       "executed"},
           {"pkg.nsless.mod", "no package named 'pkg.nsless'"},
           {"pkg..tool", "is not a module name"},
           {"class", "is not a module name"}}) {
    diagnostics.clear();
    EXPECT_FALSE(project->mainModule(name, diag)) << name;
    EXPECT_NE(diagnostics.find(expected), std::string::npos)
        << name << "\n-> " << diagnostics;
  }
}

TEST(ProjectTest, AScriptIsWhatAnInstallerWritesForIt) {
  std::string error;
  auto entry = lython::driver::EntryPoint::parse("pkg.cli:App.main", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_EQ(lython::driver::Project::scriptSource(*entry),
            "import sys\nfrom pkg.cli import App\nsys.exit(App.main())\n");
  // An object called `sys` cannot take the name the exit needs.
  entry = lython::driver::EntryPoint::parse("pkg:sys", error);
  ASSERT_TRUE(entry) << error;
  EXPECT_EQ(lython::driver::Project::scriptSource(*entry),
            "import sys\nfrom pkg import sys as __lython_entry_point\n"
            "sys.exit(__lython_entry_point())\n");
}

// --- Import paths -------------------------------------------------------------

bool emits(const ScratchDirectory &dir, llvm::StringRef mainRelative,
           llvm::StringRef importRoot, const lython::driver::DriverOptions &options,
           std::string &diagnostics) {
  std::string mainPath = dir.join(mainRelative);
  auto source = llvm::MemoryBuffer::getFile(mainPath);
  EXPECT_TRUE(static_cast<bool>(source)) << mainPath;
  if (!source)
    return false;
  mlir::DialectRegistry registry;
  lython::driver::registerLythonDialects(registry);
  mlir::MLIRContext context(registry);
  mlir::OwningOpRef<mlir::ModuleOp> module;
  llvm::raw_string_ostream diag(diagnostics);
  return mlir::succeeded(lython::driver::emitMLIRFromSource(
      (*source)->getBuffer(), mainPath, importRoot, options, context, module,
      diag));
}

// A package's modules import each other by their full names, which start from
// the import root and not from the importing module's directory.
TEST(ImportPathTest, APackageModuleImportsItsSiblingByItsFullName) {
  ScratchDirectory dir;
  dir.write("pkg/__init__.py", "N = 1\n");
  dir.write("pkg/b.py", "def f() -> int:\n    return 2\n");
  dir.write("pkg/a.py", "import pkg.b\nfrom pkg.b import f\nfrom pkg import N\n\n"
                        "def g() -> int:\n    return f() + pkg.b.f() + N\n");
  dir.write("main.py", "from pkg.a import g\nprint(g())\n");
  std::string diagnostics;
  EXPECT_TRUE(emits(dir, "main.py", dir.path, {}, diagnostics)) << diagnostics;
}

// ...and so a bare `import b` inside the package is not its sibling: that was
// Python 2's implicit relative import, and CPython 3 raises
// ModuleNotFoundError for it.
TEST(ImportPathTest, ABareImportInAPackageIsNotItsSibling) {
  ScratchDirectory dir;
  dir.write("pkg/__init__.py", "");
  dir.write("pkg/b.py", "def f() -> int:\n    return 2\n");
  dir.write("pkg/a.py", "import b\n\ndef g() -> int:\n    return b.f()\n");
  dir.write("main.py", "from pkg.a import g\nprint(g())\n");
  std::string diagnostics;
  EXPECT_FALSE(emits(dir, "main.py", dir.path, {}, diagnostics));
  EXPECT_NE(diagnostics.find("unsupported import 'b'"), std::string::npos)
      << diagnostics;
}

// `__main__` run as a member of a package (`python -m pkg`) imports relative
// to that package, from its own directory, and absolutely from the import
// root above it.
TEST(ImportPathTest, AMainModuleOfAPackageImportsBothWays) {
  ScratchDirectory dir;
  dir.write("src/pkg/__init__.py", "");
  dir.write("src/pkg/cli.py", "def main() -> int:\n    return 0\n");
  dir.write("src/pkg/util.py", "def helper() -> int:\n    return 1\n");
  dir.write("src/pkg/__main__.py",
            "from .cli import main\nfrom pkg.util import helper\n"
            "print(main() + helper())\n");
  lython::driver::DriverOptions options;
  options.mainPackage = "pkg";
  std::string diagnostics;
  EXPECT_TRUE(emits(dir, "src/pkg/__main__.py", dir.join("src"), options,
                    diagnostics))
      << diagnostics;
}

} // namespace
