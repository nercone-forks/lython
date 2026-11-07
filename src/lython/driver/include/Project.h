#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lython::driver {

// An object reference of an entry point, `importable.module:object.attr`
// (packaging.python.org/en/latest/specifications/entry-points/).
struct EntryPoint {
  std::string module;
  // `object.attr` split at the dots; empty when the reference names only the
  // module.
  std::vector<std::string> attributes;

  // Reads `module[:object.attr] [extras]`. Extras are accepted and ignored,
  // as the specification lets a consumer do. Null, with the reason in
  // `error`, when `text` is not such a reference.
  static std::optional<EntryPoint> parse(llvm::StringRef text,
                                         std::string &error);

  // `module:object.attr`.
  std::string spelling() const;
};

// What `python -m` runs for a module: the file, and the package `__main__`
// belongs to (`pkg` for `-m pkg` with `pkg/__main__.py`, and for
// `-m pkg.mod`).
struct MainModule {
  std::string sourcePath;
  std::string package;
};

// A project directory: a `pyproject.toml` (PEP 518) and its sources, either
// under `src/` (the src layout) or beside the file (the flat layout).
class Project {
public:
  // Reads `<directory>/pyproject.toml`; every path the project answers is
  // absolute and normalized. Null, with the reason written to
  // `diag`, when it is missing or is not a valid project file.
  static std::optional<Project> load(llvm::StringRef directory,
                                     llvm::raw_ostream &diag);

  // Whether `directory` holds a `pyproject.toml`.
  static bool isProjectDirectory(llvm::StringRef directory);

  const std::string &root() const { return rootDirectory; }
  // The directory the project's top-level packages are in: what an install
  // puts on sys.path.
  const std::string &importRoot() const { return importDirectory; }
  // `[project] name`; empty when the file has no `[project]` table.
  const std::string &name() const { return projectName; }
  // `[project.scripts]` and then `[project.gui-scripts]`, in file order.
  const std::vector<std::pair<std::string, EntryPoint>> &scripts() const {
    return scriptEntries;
  }
  const EntryPoint *script(llvm::StringRef name) const;

  // The file `python -m <moduleName>` runs from the import root. Null, with
  // the reason written to `diag`, when there is no such module.
  std::optional<MainModule> mainModule(llvm::StringRef moduleName,
                                       llvm::raw_ostream &diag) const;

  // The `__main__` an installer writes for a console script: import the
  // object and exit with what calling it returns.
  static std::string scriptSource(const EntryPoint &entry);
  // The script's sys.argv[0]: `<root>/<name>`, as an installed script sits in
  // the environment's bin directory under its name.
  std::string scriptPath(llvm::StringRef name) const;
  // The name the script's source is compiled under, which its traceback
  // frames show: `<root>/<name>-script.py`, as setuptools names the file
  // beside a Windows launcher.
  // ⛔ Not `scriptPath`: a frame is told apart from a runtime manifest's by
  // its `.py` suffix (findPythonSourceLoc), so the `<module>` frame was
  // dropped. Not `<name>.py` either: in a flat layout that can be a real
  // module the script imports, which would then be taken for `__main__`.
  // The hyphen keeps this one from naming any importable module.
  std::string scriptSourcePath(llvm::StringRef name) const;

private:
  std::string rootDirectory;
  std::string importDirectory;
  std::string projectName;
  std::vector<std::pair<std::string, EntryPoint>> scriptEntries;
};

} // namespace lython::driver
