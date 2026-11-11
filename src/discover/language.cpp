/*
 * language.c — Language detection from filename and extension.
 *
 * Maps file extensions and special filenames to CBMLanguage enum values.
 * Handles .m disambiguation (Objective-C vs Magma vs MATLAB).
 * Consults the process-global user config (set via cbm_set_user_lang_config)
 * before the built-in lookup table.
 */
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "cbm.h" // CBMLanguage, CBM_LANG_*

#include "foundation/constants.h"
#include "foundation/compat_fs.h"

enum { LANG_SCAN_PASSES = 2 };
#define SLEN(s) (sizeof(s) - 1)
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <array>

/* ── Extension → Language lookup table ───────────────────────────── */

typedef struct {
    const char *ext; /* including dot, e.g. ".go" */
    CBMLanguage language;
} ext_entry_t;

/* Sorted by extension for binary search (but linear scan is fine for ~120 entries) */
static const ext_entry_t EXT_TABLE[] = {
    /* Bash */
    {".bash", CBM_LANG_BASH},
    {".sh", CBM_LANG_BASH},

    /* C */
    {".c", CBM_LANG_C},

    /* C++ */
    {".cc", CBM_LANG_CPP},
    {".ccm", CBM_LANG_CPP},
    {".cpp", CBM_LANG_CPP},
    {".cppm", CBM_LANG_CPP},
    {".cxx", CBM_LANG_CPP},
    {".h", CBM_LANG_CPP},
    {".hh", CBM_LANG_CPP},
    {".hpp", CBM_LANG_CPP},
    {".hxx", CBM_LANG_CPP},
    {".ixx", CBM_LANG_CPP},

    /* C# */
    {".cs", CBM_LANG_CSHARP},
    /* Blazor components and Razor Pages / MVC views. The C# grammar recovers
     * the @code / @{ } / @functions blocks; the surrounding markup parses as
     * ERROR regions and is reported via parse_partial, which is why this is a
     * best-effort mapping rather than a dedicated grammar. The `@page`
     * directive that routes them is read from raw source in extract_defs. */
    {".razor", CBM_LANG_CSHARP},
    {".cshtml", CBM_LANG_CSHARP},

    /* Clojure */
    {".clj", CBM_LANG_CLOJURE},
    {".cljc", CBM_LANG_CLOJURE},
    {".cljs", CBM_LANG_CLOJURE},

    /* CMake */
    {".cmake", CBM_LANG_CMAKE},

    /* COBOL */
    {".cbl", CBM_LANG_COBOL},
    {".cob", CBM_LANG_COBOL},

    /* Common Lisp */
    {".cl", CBM_LANG_COMMONLISP},
    {".lisp", CBM_LANG_COMMONLISP},
    {".lsp", CBM_LANG_COMMONLISP},

    /* CSS */
    {".css", CBM_LANG_CSS},

    /* CUDA */
    {".cu", CBM_LANG_CUDA},
    {".cuh", CBM_LANG_CUDA},

    /* Dart */
    {".dart", CBM_LANG_DART},

    /* Dockerfile */
    {".dockerfile", CBM_LANG_DOCKERFILE},

    /* Elixir */
    {".ex", CBM_LANG_ELIXIR},
    {".exs", CBM_LANG_ELIXIR},

    /* DotEnv */
    {".env", CBM_LANG_DOTENV},

    /* Elm */
    {".elm", CBM_LANG_ELM},

    /* Emacs Lisp */
    {".el", CBM_LANG_EMACSLISP},

    /* Erlang */
    {".erl", CBM_LANG_ERLANG},

    /* F# */
    {".fs", CBM_LANG_FSHARP},
    {".fsi", CBM_LANG_FSHARP},
    {".fsx", CBM_LANG_FSHARP},

    /* FORM */
    {".frm", CBM_LANG_FORM},
    {".prc", CBM_LANG_FORM},

    /* Fortran */
    {".f03", CBM_LANG_FORTRAN},
    {".f08", CBM_LANG_FORTRAN},
    {".f90", CBM_LANG_FORTRAN},
    {".f95", CBM_LANG_FORTRAN},

    /* GLSL */
    {".frag", CBM_LANG_GLSL},
    {".glsl", CBM_LANG_GLSL},
    {".vert", CBM_LANG_GLSL},

    /* Go */
    {".go", CBM_LANG_GO},

    /* GraphQL */
    {".gql", CBM_LANG_GRAPHQL},
    {".graphql", CBM_LANG_GRAPHQL},

    /* Groovy */
    {".gradle", CBM_LANG_GROOVY},
    {".groovy", CBM_LANG_GROOVY},

    /* Haskell */
    {".hs", CBM_LANG_HASKELL},

    /* HCL / Terraform */
    {".hcl", CBM_LANG_HCL},
    {".tf", CBM_LANG_HCL},

    /* HTML */
    {".htm", CBM_LANG_HTML},
    {".html", CBM_LANG_HTML},

    /* INI */
    {".cfg", CBM_LANG_INI},
    {".conf", CBM_LANG_INI},
    {".ini", CBM_LANG_INI},

    /* Java */
    {".java", CBM_LANG_JAVA},

    /* JavaScript */
    {".js", CBM_LANG_JAVASCRIPT},
    {".jsx", CBM_LANG_JAVASCRIPT},
    {".mjs", CBM_LANG_JAVASCRIPT}, /* ES modules (#197) */
    {".cjs", CBM_LANG_JAVASCRIPT}, /* CommonJS modules */

    /* JSON */
    {".json", CBM_LANG_JSON},

    /* Julia */
    {".jl", CBM_LANG_JULIA},

    /* Kotlin */
    {".kt", CBM_LANG_KOTLIN},
    {".kts", CBM_LANG_KOTLIN},

    /* Lean */
    {".lean", CBM_LANG_LEAN},

    /* Lua */
    {".lua", CBM_LANG_LUA},

    /* Magma */
    {".mag", CBM_LANG_MAGMA},
    {".magma", CBM_LANG_MAGMA},

    /* Makefile */
    {".mk", CBM_LANG_MAKEFILE},

    /* Markdown */
    {".md", CBM_LANG_MARKDOWN},
    {".mdx", CBM_LANG_MARKDOWN},

    /* MATLAB */
    {".m", CBM_LANG_MATLAB},
    {".matlab", CBM_LANG_MATLAB},
    {".mlx", CBM_LANG_MATLAB},

    /* Meson */
    {".meson", CBM_LANG_MESON},

    /* Mojo */
    {".mojo", CBM_LANG_MOJO},

    /* Nix */
    {".nix", CBM_LANG_NIX},

    /* OCaml */
    {".ml", CBM_LANG_OCAML},
    {".mli", CBM_LANG_OCAML},

    /* Perl */
    {".pl", CBM_LANG_PERL},
    {".pm", CBM_LANG_PERL},

    /* PHP */
    {".php", CBM_LANG_PHP},

    /* Protobuf */
    {".proto", CBM_LANG_PROTOBUF},

    /* Python */
    {".py", CBM_LANG_PYTHON},

    /* R — case insensitive handled separately */
    {".R", CBM_LANG_R},
    {".r", CBM_LANG_R},

    /* Ruby */
    {".gemspec", CBM_LANG_RUBY},
    {".rake", CBM_LANG_RUBY},
    {".rb", CBM_LANG_RUBY},

    /* Rust */
    {".rs", CBM_LANG_RUST},

    /* Scala */
    {".sc", CBM_LANG_SCALA},
    {".scala", CBM_LANG_SCALA},

    /* SCSS */
    {".scss", CBM_LANG_SCSS},

    /* SQL */
    {".sql", CBM_LANG_SQL},

    /* Svelte */
    {".svelte", CBM_LANG_SVELTE},

    /* Swift */
    {".swift", CBM_LANG_SWIFT},

    /* SystemVerilog + Verilog */
    {".sv", CBM_LANG_VERILOG},
    {".v", CBM_LANG_VERILOG},

    /* TOML */
    {".toml", CBM_LANG_TOML},

    /* TSX */
    {".tsx", CBM_LANG_TSX},

    /* TypeScript */
    {".ts", CBM_LANG_TYPESCRIPT},
    {".mts", CBM_LANG_TYPESCRIPT}, /* TS ES modules */
    {".cts", CBM_LANG_TYPESCRIPT}, /* TS CommonJS modules */

    /* VimScript */
    {".vim", CBM_LANG_VIMSCRIPT},
    {".vimrc", CBM_LANG_VIMSCRIPT},
    {"justfile", CBM_LANG_JUST},
    {"Justfile", CBM_LANG_JUST},
    {".justfile", CBM_LANG_JUST},
    {".just", CBM_LANG_JUST}, /* `import 'common.just'` target files */
    {"hyprland.conf", CBM_LANG_HYPRLANG},
    {"ssh_config", CBM_LANG_SSHCONFIG},
    {"sshd_config", CBM_LANG_SSHCONFIG},
    {"BUILD", CBM_LANG_STARLARK},
    {"BUILD.bazel", CBM_LANG_STARLARK},
    {"WORKSPACE", CBM_LANG_STARLARK},
    {"WORKSPACE.bazel", CBM_LANG_STARLARK},

    /* BitBake include fragments — `require/include foo.inc` target files.
     * Discovery refines this by content: C-family fragments become C++
     * (cbm_disambiguate_inc). */
    {".inc", CBM_LANG_BITBAKE},

    /* Vue */
    {".vue", CBM_LANG_VUE},

    /* Wolfram */
    {".wl", CBM_LANG_WOLFRAM},
    {".wls", CBM_LANG_WOLFRAM},

    /* XML */
    {".xml", CBM_LANG_XML},
    {".xsd", CBM_LANG_XML},
    {".xsl", CBM_LANG_XML},
    {".svg", CBM_LANG_XML},

    /* YAML */
    {".yaml", CBM_LANG_YAML},
    {".yml", CBM_LANG_YAML},

    /* Ada */
    {".adb", CBM_LANG_ADA},

    /* Ada */
    {".ads", CBM_LANG_ADA},

    /* Agda */
    {".agda", CBM_LANG_AGDA},

    /* Astro */
    {".astro", CBM_LANG_ASTRO},

    /* AWK */
    {".awk", CBM_LANG_AWK},

    /* BitBake */
    {".bb", CBM_LANG_BITBAKE},

    /* BitBake */
    {".bbappend", CBM_LANG_BITBAKE},

    /* BitBake */
    {".bbclass", CBM_LANG_BITBAKE},

    /* Beancount */
    {".beancount", CBM_LANG_BEANCOUNT},

    /* BibTeX */
    {".bib", CBM_LANG_BIBTEX},

    /* Bicep */
    {".bicep", CBM_LANG_BICEP},

    /* Blade */
    /* .blade.php handled by userconfig compound extensions, not EXT_TABLE */

    /* Starlark */
    {".bzl", CBM_LANG_STARLARK},

    /* Cairo */
    {".cairo", CBM_LANG_CAIRO},

    /* Cap'n Proto */
    {".capnp", CBM_LANG_CAPNP},

    /* Apex */
    {".cls", CBM_LANG_APEX},

    /* Crystal */
    {".cr", CBM_LANG_CRYSTAL},

    /* CSV */
    {".csv", CBM_LANG_CSV},

    /* D (discovery drops make/cargo dep-info .d files: cbm_disambiguate_d) */
    {".d", CBM_LANG_DLANG},

    /* Diff */
    {".diff", CBM_LANG_DIFF},

    /* Pascal */
    {".dpr", CBM_LANG_PASCAL},

    /* DeviceTree */
    {".dts", CBM_LANG_DEVICETREE},

    /* DeviceTree */
    {".dtsi", CBM_LANG_DEVICETREE},

    /* FunC */
    {".fc", CBM_LANG_FUNC},

    /* Fish */
    {".fish", CBM_LANG_FISH},

    /* Fennel */
    {".fnl", CBM_LANG_FENNEL},

    /* HLSL */
    {".fx", CBM_LANG_HLSL},

    /* GDScript */
    {".gd", CBM_LANG_GDSCRIPT},

    /* Gleam */
    {".gleam", CBM_LANG_GLEAM},

    /* GN */
    {".gn", CBM_LANG_GN},

    /* GN */
    {".gni", CBM_LANG_GN},

    /* Go Template */
    {".gotmpl", CBM_LANG_GOTEMPLATE},
    {".tpl", CBM_LANG_GOTEMPLATE}, /* Helm _helpers.tpl named-template definitions */

    /* Hare */
    {".ha", CBM_LANG_HARE},

    /* Hyprlang */
    {".hl", CBM_LANG_HYPRLANG},

    /* HLSL */
    {".hlsl", CBM_LANG_HLSL},

    /* HLSL */
    {".hlsli", CBM_LANG_HLSL},

    /* ISPC */
    {".ispc", CBM_LANG_ISPC},

    /* Jinja2 */
    {".j2", CBM_LANG_JINJA2},

    /* Janet */
    {".janet", CBM_LANG_JANET},

    /* Jinja2 */
    {".jinja", CBM_LANG_JINJA2},

    /* Jinja2 */
    {".jinja2", CBM_LANG_JINJA2},

    /* JSON5 */
    {".json5", CBM_LANG_JSON5},

    /* Jsonnet */
    {".jsonnet", CBM_LANG_JSONNET},

    /* KDL */
    {".kdl", CBM_LANG_KDL},

    /* Linker Script */
    {".ld", CBM_LANG_LINKERSCRIPT},

    /* Linker Script */
    {".lds", CBM_LANG_LINKERSCRIPT},

    /* Jsonnet */
    {".libsonnet", CBM_LANG_JSONNET},

    /* Liquid */
    {".liquid", CBM_LANG_LIQUID},

    /* LLVM IR */
    {".ll", CBM_LANG_LLVM_IR},

    /* Pascal */
    {".lpr", CBM_LANG_PASCAL},

    /* Luau */
    {".luau", CBM_LANG_LUAU},

    /* Mermaid */
    {".mermaid", CBM_LANG_MERMAID},

    /* Mermaid */
    {".mmd", CBM_LANG_MERMAID},

    /* Move */
    {".move", CBM_LANG_MOVE},

    /* NASM */
    {".nasm", CBM_LANG_NASM},

    /* Nickel */
    {".ncl", CBM_LANG_NICKEL},

    /* Nim */

    /* Nim */

    /* Squirrel */
    {".nut", CBM_LANG_SQUIRREL},

    /* Odin */
    {".odin", CBM_LANG_ODIN},

    /* DeviceTree */
    {".overlay", CBM_LANG_DEVICETREE},

    /* Pascal */
    {".pas", CBM_LANG_PASCAL},

    /* Diff */
    {".patch", CBM_LANG_DIFF},

    /* Pine Script */
    {".pine", CBM_LANG_PINE},

    /* Pkl */
    {".pkl", CBM_LANG_PKL},

    /* PO */
    {".po", CBM_LANG_PO},

    /* Pony */
    {".pony", CBM_LANG_PONY},

    /* PO */
    {".pot", CBM_LANG_PO},

    /* Puppet */
    {".pp", CBM_LANG_PUPPET},

    /* Prisma */
    {".prisma", CBM_LANG_PRISMA},

    /* Properties */
    {".properties", CBM_LANG_PROPERTIES},

    /* PowerShell */
    {".ps1", CBM_LANG_POWERSHELL},

    /* PowerShell */
    {".psd1", CBM_LANG_POWERSHELL},

    /* PowerShell */
    {".psm1", CBM_LANG_POWERSHELL},

    /* PureScript */
    {".purs", CBM_LANG_PURESCRIPT},

    /* ReScript */
    {".res", CBM_LANG_RESCRIPT},

    /* ReScript */
    {".resi", CBM_LANG_RESCRIPT},

    /* Regex */
    {".re", CBM_LANG_REGEX},

    /* Racket */
    {".rkt", CBM_LANG_RACKET},

    /* RON */
    {".ron", CBM_LANG_RON},

    /* reStructuredText */
    {".rst", CBM_LANG_RST},

    /* Assembly */
    {".s", CBM_LANG_ASSEMBLY},

    /* Assembly */
    {".S", CBM_LANG_ASSEMBLY},

    /* Scheme */
    {".scm", CBM_LANG_SCHEME},

    /* Slang */
    {".slang", CBM_LANG_SLANG},

    /* Smali */
    {".smali", CBM_LANG_SMALI},

    /* Smithy */
    {".smithy", CBM_LANG_SMITHY},

    /* Solidity */
    {".sol", CBM_LANG_SOLIDITY},

    /* SOQL */
    {".soql", CBM_LANG_SOQL},

    /* SOSL */
    {".sosl", CBM_LANG_SOSL},

    /* Scheme */
    {".ss", CBM_LANG_SCHEME},

    /* Starlark */
    {".star", CBM_LANG_STARLARK},

    /* SystemVerilog */

    /* SystemVerilog */

    /* Sway */
    {".sw", CBM_LANG_SWAY},

    /* Tcl */
    {".tcl", CBM_LANG_TCL},

    /* TableGen */
    {".td", CBM_LANG_TABLEGEN},

    /* Templ */
    {".templ", CBM_LANG_TEMPL},

    /* Thrift */
    {".thrift", CBM_LANG_THRIFT},

    /* Teal */
    {".tl", CBM_LANG_TEAL},

    /* TLA+ */
    {".tla", CBM_LANG_TLAPLUS},

    /* Go Template */
    {".tmpl", CBM_LANG_GOTEMPLATE},

    /* Apex */
    {".trigger", CBM_LANG_APEX},

    /* Typst */
    {".typ", CBM_LANG_TYPST},

    /* VHDL */
    {".vhd", CBM_LANG_VHDL},

    /* VHDL */
    {".vhdl", CBM_LANG_VHDL},

    /* WGSL */
    {".wgsl", CBM_LANG_WGSL},

    /* WIT */
    {".wit", CBM_LANG_WIT},

    /* Zsh */
    {".zsh", CBM_LANG_ZSH},

    /* Zig */
    {".zig", CBM_LANG_ZIG},
};

#define EXT_TABLE_SIZE (sizeof(EXT_TABLE) / sizeof(EXT_TABLE[0]))

/* ── Special filename → Language lookup ──────────────────────────── */

typedef struct {
    const char *filename;
    CBMLanguage language;
} filename_entry_t;

static const filename_entry_t FILENAME_TABLE[] = {
    {"CMakeLists.txt", CBM_LANG_CMAKE},
    {"Dockerfile", CBM_LANG_DOCKERFILE},
    {"GNUmakefile", CBM_LANG_MAKEFILE},
    {"Makefile", CBM_LANG_MAKEFILE},
    {"makefile", CBM_LANG_MAKEFILE},
    {"meson.build", CBM_LANG_MESON},
    {"meson.options", CBM_LANG_MESON},
    {"meson_options.txt", CBM_LANG_MESON},
    {"kustomization.yaml", CBM_LANG_KUSTOMIZE},
    {"kustomization.yml", CBM_LANG_KUSTOMIZE},
    /* Note: FILENAME_TABLE uses case-sensitive strcmp, so mixed-case variants
     * (e.g. "Kustomization.yaml") are not matched here.  They fall through to
     * CBM_LANG_YAML and are re-classified by cbm_is_kustomize_file() in
     * pass_k8s.c, which performs a case-insensitive comparison.  This is the
     * intended behaviour — no additional entries are needed. */
    {".vimrc", CBM_LANG_VIMSCRIPT},
    {".zshrc", CBM_LANG_ZSH},
    {".zshenv", CBM_LANG_ZSH},
    {".zprofile", CBM_LANG_ZSH},
    {"justfile", CBM_LANG_JUST},
    {"Justfile", CBM_LANG_JUST},
    {".justfile", CBM_LANG_JUST},
    {"hyprland.conf", CBM_LANG_HYPRLANG},
    {"ssh_config", CBM_LANG_SSHCONFIG},
    {"sshd_config", CBM_LANG_SSHCONFIG},
    {".ssh/config", CBM_LANG_SSHCONFIG},
    {"BUILD", CBM_LANG_STARLARK},
    {"BUILD.bazel", CBM_LANG_STARLARK},
    {"WORKSPACE", CBM_LANG_STARLARK},
    {"WORKSPACE.bazel", CBM_LANG_STARLARK},
    {"requirements.txt", CBM_LANG_REQUIREMENTS},
    {"requirements-dev.txt", CBM_LANG_REQUIREMENTS},
    {"requirements-test.txt", CBM_LANG_REQUIREMENTS},
    {"Kconfig", CBM_LANG_KCONFIG},
    {"go.mod", CBM_LANG_GOMOD},
    {".env", CBM_LANG_DOTENV},
    {".env.local", CBM_LANG_DOTENV},
    {".gitattributes", CBM_LANG_GITATTRIBUTES},

};

#define FILENAME_TABLE_SIZE (sizeof(FILENAME_TABLE) / sizeof(FILENAME_TABLE[0]))

/* ── Language names ──────────────────────────────────────────────── */

/* Sparse name table: built once as a function-local static (C++ has no
 * array designators). Gaps remain nullptr -> reported as "Unknown". */
static const std::array<const char *, CBM_LANG_COUNT> &lang_names() {
    static const std::array<const char *, CBM_LANG_COUNT> tbl = [] {
        std::array<const char *, CBM_LANG_COUNT> a{}; /* value-init: all nullptr */
        a[CBM_LANG_GO] = "Go";
        a[CBM_LANG_PYTHON] = "Python";
        a[CBM_LANG_JAVASCRIPT] = "JavaScript";
        a[CBM_LANG_TYPESCRIPT] = "TypeScript";
        a[CBM_LANG_TSX] = "TSX";
        a[CBM_LANG_RUST] = "Rust";
        a[CBM_LANG_JAVA] = "Java";
        a[CBM_LANG_CPP] = "C++";
        a[CBM_LANG_CSHARP] = "C#";
        a[CBM_LANG_PHP] = "PHP";
        a[CBM_LANG_LUA] = "Lua";
        a[CBM_LANG_SCALA] = "Scala";
        a[CBM_LANG_KOTLIN] = "Kotlin";
        a[CBM_LANG_RUBY] = "Ruby";
        a[CBM_LANG_C] = "C";
        a[CBM_LANG_BASH] = "Bash";
        a[CBM_LANG_ZIG] = "Zig";
        a[CBM_LANG_ELIXIR] = "Elixir";
        a[CBM_LANG_HASKELL] = "Haskell";
        a[CBM_LANG_OCAML] = "OCaml";
        a[CBM_LANG_OBJC] = "Objective-C";
        a[CBM_LANG_SWIFT] = "Swift";
        a[CBM_LANG_DART] = "Dart";
        a[CBM_LANG_PERL] = "Perl";
        a[CBM_LANG_GROOVY] = "Groovy";
        a[CBM_LANG_ERLANG] = "Erlang";
        a[CBM_LANG_R] = "R";
        a[CBM_LANG_HTML] = "HTML";
        a[CBM_LANG_CSS] = "CSS";
        a[CBM_LANG_SCSS] = "SCSS";
        a[CBM_LANG_YAML] = "YAML";
        a[CBM_LANG_TOML] = "TOML";
        a[CBM_LANG_HCL] = "HCL";
        a[CBM_LANG_SQL] = "SQL";
        a[CBM_LANG_DOCKERFILE] = "Dockerfile";
        a[CBM_LANG_CLOJURE] = "Clojure";
        a[CBM_LANG_FSHARP] = "F#";
        a[CBM_LANG_JULIA] = "Julia";
        a[CBM_LANG_VIMSCRIPT] = "VimScript";
        a[CBM_LANG_NIX] = "Nix";
        a[CBM_LANG_COMMONLISP] = "Common Lisp";
        a[CBM_LANG_ELM] = "Elm";
        a[CBM_LANG_FORTRAN] = "Fortran";
        a[CBM_LANG_CUDA] = "CUDA";
        a[CBM_LANG_COBOL] = "COBOL";
        a[CBM_LANG_VERILOG] = "Verilog";
        a[CBM_LANG_EMACSLISP] = "Emacs Lisp";
        a[CBM_LANG_JSON] = "JSON";
        a[CBM_LANG_XML] = "XML";
        a[CBM_LANG_MARKDOWN] = "Markdown";
        a[CBM_LANG_MAKEFILE] = "Makefile";
        a[CBM_LANG_CMAKE] = "CMake";
        a[CBM_LANG_PROTOBUF] = "Protobuf";
        a[CBM_LANG_GRAPHQL] = "GraphQL";
        a[CBM_LANG_VUE] = "Vue";
        a[CBM_LANG_SVELTE] = "Svelte";
        a[CBM_LANG_MESON] = "Meson";
        a[CBM_LANG_GLSL] = "GLSL";
        a[CBM_LANG_INI] = "INI";
        a[CBM_LANG_MATLAB] = "MATLAB";
        a[CBM_LANG_LEAN] = "Lean";
        a[CBM_LANG_FORM] = "FORM";
        a[CBM_LANG_MAGMA] = "Magma";
        a[CBM_LANG_WOLFRAM] = "Wolfram";
        a[CBM_LANG_KUSTOMIZE] = "Kustomize";
        a[CBM_LANG_K8S] = "Kubernetes";
        a[CBM_LANG_PINE] = "PineScript";
        a[CBM_LANG_SOLIDITY] = "Solidity";
        a[CBM_LANG_TYPST] = "Typst";
        a[CBM_LANG_GDSCRIPT] = "GDScript";
        a[CBM_LANG_GLEAM] = "Gleam";
        a[CBM_LANG_POWERSHELL] = "PowerShell";
        a[CBM_LANG_PASCAL] = "Pascal";
        a[CBM_LANG_DLANG] = "D";
        a[CBM_LANG_NIM] = "Nim";
        a[CBM_LANG_SCHEME] = "Scheme";
        a[CBM_LANG_FENNEL] = "Fennel";
        a[CBM_LANG_FISH] = "Fish";
        a[CBM_LANG_AWK] = "AWK";
        a[CBM_LANG_ZSH] = "Zsh";
        a[CBM_LANG_TCL] = "Tcl";
        a[CBM_LANG_ADA] = "Ada";
        a[CBM_LANG_AGDA] = "Agda";
        a[CBM_LANG_RACKET] = "Racket";
        a[CBM_LANG_ODIN] = "Odin";
        a[CBM_LANG_RESCRIPT] = "ReScript";
        a[CBM_LANG_PURESCRIPT] = "PureScript";
        a[CBM_LANG_NICKEL] = "Nickel";
        a[CBM_LANG_CRYSTAL] = "Crystal";
        a[CBM_LANG_TEAL] = "Teal";
        a[CBM_LANG_HARE] = "Hare";
        a[CBM_LANG_PONY] = "Pony";
        a[CBM_LANG_LUAU] = "Luau";
        a[CBM_LANG_JANET] = "Janet";
        a[CBM_LANG_SWAY] = "Sway";
        a[CBM_LANG_NASM] = "NASM";
        a[CBM_LANG_ASSEMBLY] = "Assembly";
        a[CBM_LANG_ASTRO] = "Astro";
        a[CBM_LANG_BLADE] = "Blade";
        a[CBM_LANG_JUST] = "Just";
        a[CBM_LANG_GOTEMPLATE] = "Go Template";
        a[CBM_LANG_TEMPL] = "Templ";
        a[CBM_LANG_LIQUID] = "Liquid";
        a[CBM_LANG_JINJA2] = "Jinja2";
        a[CBM_LANG_PRISMA] = "Prisma";
        a[CBM_LANG_HYPRLANG] = "Hyprlang";
        a[CBM_LANG_DOTENV] = "DotEnv";
        a[CBM_LANG_SYSTEMVERILOG] = "SystemVerilog";
        a[CBM_LANG_DIFF] = "Diff";
        a[CBM_LANG_WGSL] = "WGSL";
        a[CBM_LANG_KDL] = "KDL";
        a[CBM_LANG_JSON5] = "JSON5";
        a[CBM_LANG_JSONNET] = "Jsonnet";
        a[CBM_LANG_RON] = "RON";
        a[CBM_LANG_THRIFT] = "Thrift";
        a[CBM_LANG_CAPNP] = "Cap'n Proto";
        a[CBM_LANG_PROPERTIES] = "Properties";
        a[CBM_LANG_SSHCONFIG] = "SSH Config";
        a[CBM_LANG_BIBTEX] = "BibTeX";
        a[CBM_LANG_STARLARK] = "Starlark";
        a[CBM_LANG_BICEP] = "Bicep";
        a[CBM_LANG_CSV] = "CSV";
        a[CBM_LANG_REQUIREMENTS] = "Requirements";
        a[CBM_LANG_HLSL] = "HLSL";
        a[CBM_LANG_VHDL] = "VHDL";
        a[CBM_LANG_DEVICETREE] = "DeviceTree";
        a[CBM_LANG_LINKERSCRIPT] = "Linker Script";
        a[CBM_LANG_GN] = "GN";
        a[CBM_LANG_KCONFIG] = "Kconfig";
        a[CBM_LANG_BITBAKE] = "BitBake";
        a[CBM_LANG_SMALI] = "Smali";
        a[CBM_LANG_TABLEGEN] = "TableGen";
        a[CBM_LANG_ISPC] = "ISPC";
        a[CBM_LANG_CAIRO] = "Cairo";
        a[CBM_LANG_MOVE] = "Move";
        a[CBM_LANG_SQUIRREL] = "Squirrel";
        a[CBM_LANG_FUNC] = "FunC";
        a[CBM_LANG_REGEX] = "Regex";
        a[CBM_LANG_JSDOC] = "JSDoc";
        a[CBM_LANG_RST] = "reStructuredText";
        a[CBM_LANG_BEANCOUNT] = "Beancount";
        a[CBM_LANG_MERMAID] = "Mermaid";
        a[CBM_LANG_PUPPET] = "Puppet";
        a[CBM_LANG_PO] = "PO";
        a[CBM_LANG_GITATTRIBUTES] = "gitattributes";
        a[CBM_LANG_GITIGNORE] = "gitignore";
        a[CBM_LANG_SLANG] = "Slang";
        a[CBM_LANG_LLVM_IR] = "LLVM IR";
        a[CBM_LANG_SMITHY] = "Smithy";
        a[CBM_LANG_WIT] = "WIT";
        a[CBM_LANG_TLAPLUS] = "TLA+";
        a[CBM_LANG_PKL] = "Pkl";
        a[CBM_LANG_GOMOD] = "Go Mod";
        a[CBM_LANG_APEX] = "Apex";
        a[CBM_LANG_SOQL] = "SOQL";
        a[CBM_LANG_SOSL] = "SOSL";
        a[CBM_LANG_MOJO] = "Mojo";
        return a;
    }();
    return tbl;
}

/* ── Public API ──────────────────────────────────────────────────── */

CBMLanguage cbm_language_for_extension(const char *ext) {
    if (!ext || !ext[0]) {
        return CBM_LANG_COUNT;
    }

    /* Check user-defined overrides first */
    const cbm_userconfig_t *ucfg = cbm_get_user_lang_config();
    if (ucfg) {
        CBMLanguage ulang = cbm_userconfig_lookup(ucfg, ext);
        if (ulang != CBM_LANG_COUNT) {
            return ulang;
        }
    }

    for (size_t i = 0; i < EXT_TABLE_SIZE; i++) {
        if (strcmp(EXT_TABLE[i].ext, ext) == 0) {
            return EXT_TABLE[i].language;
        }
    }
    return CBM_LANG_COUNT;
}

CBMLanguage cbm_language_for_filename(const char *filename) {
    if (!filename || !filename[0]) {
        return CBM_LANG_COUNT;
    }

    /* Check special filenames first */
    for (size_t i = 0; i < FILENAME_TABLE_SIZE; i++) {
        if (strcmp(FILENAME_TABLE[i].filename, filename) == 0) {
            return FILENAME_TABLE[i].language;
        }
    }

    /* DotEnv variant filenames (".env.local", ".env.production", …): the
     * filename starts with ".env." but its last "extension" (e.g. ".local")
     * is not a real language extension.  Match the dotenv convention used by
     * pass_envscan/pass_infrascan (".env" exact, ".env." prefix, "*.env"
     * suffix) so file-index routing agrees with direct extraction. */
    if (strncmp(filename, ".env.", SLEN(".env.")) == 0) {
        return CBM_LANG_DOTENV;
    }

    /* Fall back to extension-based lookup.
     * For compound extensions (e.g. ".blade.php") defined in the user config,
     * scan from the first dot in the basename toward the last, checking user
     * config at each position.  Built-in extensions use the last dot only. */
    const char *last_dot = strrchr(filename, '.');
    if (!last_dot) {
        return CBM_LANG_COUNT;
    }

    /* Probe compound extensions (e.g. ".blade.php") from the first dot toward
     * the last. Built-in compounds are checked first so e.g. Laravel Blade
     * templates map to Blade rather than the single-extension fallback (PHP);
     * user config can still add more (#258). */
    static const struct {
        const char *ext;
        CBMLanguage lang;
    } COMPOUND_EXT_TABLE[] = {
        {".blade.php", CBM_LANG_BLADE},
    };
    const cbm_userconfig_t *ucfg = cbm_get_user_lang_config();
    const char *p = strchr(filename, '.');
    while (p && p < last_dot) {
        for (size_t i = 0; i < sizeof(COMPOUND_EXT_TABLE) / sizeof(COMPOUND_EXT_TABLE[0]); i++) {
            if (strcmp(p, COMPOUND_EXT_TABLE[i].ext) == 0) {
                return COMPOUND_EXT_TABLE[i].lang;
            }
        }
        if (ucfg) {
            CBMLanguage lang = cbm_userconfig_lookup(ucfg, p);
            if (lang != CBM_LANG_COUNT) {
                return lang;
            }
        }
        p = strchr(p + SKIP_ONE, '.');
    }

    /* Standard single-extension lookup (built-ins + user overrides). */
    return cbm_language_for_extension(last_dot);
}

const char *cbm_language_name(CBMLanguage lang) {
    if (lang < 0 || lang >= CBM_LANG_COUNT) {
        return "Unknown";
    }
    return lang_names()[lang] ? lang_names()[lang] : "Unknown";
}

/* ── .m file disambiguation ──────────────────────────────────────── */

/* Simple substring search helper */
static bool str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static bool has_objc_markers(const char *buf) {
    return str_contains(buf, "@interface") || str_contains(buf, "@implementation") ||
           str_contains(buf, "@protocol") || str_contains(buf, "@property") ||
           str_contains(buf, "#import") || str_contains(buf, "@selector") ||
           str_contains(buf, "@encode") || str_contains(buf, "@synthesize") ||
           str_contains(buf, "@dynamic");
}

static bool has_magma_end_markers(const char *buf) {
    return str_contains(buf, "end function;") || str_contains(buf, "end procedure;") ||
           str_contains(buf, "end intrinsic;") || str_contains(buf, "end if;") ||
           str_contains(buf, "end for;") || str_contains(buf, "end while;");
}

/* Check for "intrinsic Name(" or "procedure Name(" patterns. */
static bool has_magma_callable_pattern(const char *buf) {
    const char *markers[] = {"intrinsic ", "procedure "};
    for (int i = 0; i < LANG_SCAN_PASSES; i++) {
        const char *p = strstr(buf, markers[i]);
        if (!p) {
            continue;
        }
        p += strlen(markers[i]);
        while (*p && isalpha((unsigned char)*p)) {
            p++;
        }
        if (*p == '(') {
            return true;
        }
    }
    return false;
}

/* Scan lines for MATLAB-specific markers (function/classdef/%%). */
static bool has_matlab_line_markers(const char *buf) {
    const char *line = buf;
    while (*line) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "function ", SLEN("function ")) == 0 ||
            strncmp(p, "function\t", SLEN("function\t")) == 0 ||
            strncmp(p, "classdef ", SLEN("classdef ")) == 0 ||
            strncmp(p, "classdef\t", SLEN("classdef\t")) == 0 || strncmp(p, "%%", PAIR_LEN) == 0 ||
            (*p == '%' && *(p + SKIP_ONE) != '{')) {
            return true;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) {
            break;
        }
        line = nl + SKIP_ONE;
    }
    return false;
}

CBMLanguage cbm_disambiguate_m(const char *path) {
    if (!path) {
        return CBM_LANG_MATLAB;
    }

    FILE *f = cbm_fopen(path, "r");
    if (!f) {
        return CBM_LANG_MATLAB;
    }

    /* Read first 4KB */
    char buf[CBM_SZ_4K + SKIP_ONE];
    size_t n = fread(buf, SKIP_ONE, CBM_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);

    if (has_objc_markers(buf)) {
        return CBM_LANG_OBJC;
    }
    if (has_magma_end_markers(buf)) {
        return CBM_LANG_MAGMA;
    }
    if ((str_contains(buf, "intrinsic ") || str_contains(buf, "procedure ")) &&
        has_magma_callable_pattern(buf)) {
        return CBM_LANG_MAGMA;
    }
    if (has_matlab_line_markers(buf)) {
        return CBM_LANG_MATLAB;
    }

    return CBM_LANG_MATLAB;
}

/* Visual Basic 6 / VBA source exports are recognisable from their header (#721):
 * every module carries `Attribute VB_Name = "..."`, class modules open with
 * `VERSION 1.0 CLASS`, forms/controls with `VERSION 5.00` + `Begin VB.Form` /
 * `Begin VB.UserControl`. None of these occur in Apex classes or FORM programs,
 * whose .cls / .frm extensions VB6 happens to share. */
static bool has_vb6_markers(const char *buf) {
    return str_contains(buf, "Attribute VB_Name") || str_contains(buf, "VERSION 1.0 CLASS") ||
           str_contains(buf, "Begin VB.") || str_contains(buf, "\nOption Explicit");
}

/* Read up to 4KB of `path` into `buf` (NUL-terminated). False on open failure. */
static bool read_head_4k(const char *path, char *buf) {
    FILE *f = cbm_fopen(path, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, SKIP_ONE, CBM_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);
    return true;
}

/* Disambiguate .frm files: shared by the FORM symbolic-manipulation language
 * and Visual Basic 6 forms (#721). There is no Visual Basic language yet, so a
 * VB6 form is reported as unsupported (CBM_LANG_COUNT) rather than handed to
 * the FORM grammar, which yields no defs and stray junk nodes. Defaults to
 * FORM on any doubt. */
CBMLanguage cbm_disambiguate_frm(const char *path) {
    char buf[CBM_SZ_4K + SKIP_ONE];
    if (!path || !read_head_4k(path, buf)) {
        return CBM_LANG_FORM;
    }
    /* VB6 form files open with "VERSION x.yy" on line 1. */
    if (strncmp(buf, "VERSION ", SLEN("VERSION ")) == 0 &&
        isdigit((unsigned char)buf[SLEN("VERSION ")])) {
        return CBM_LANG_COUNT;
    }
    return has_vb6_markers(buf) ? CBM_LANG_COUNT : CBM_LANG_FORM;
}

/* Disambiguate .res files (#2176): ReScript source shares the extension with
 * two binary formats -- Godot resources (RSRC/RSCC magic, the default save
 * format for imported meshes) and Windows compiled resource files. Binary
 * content has no ReScript meaning, and a NUL byte never occurs in ReScript
 * text while both binary formats carry NULs in their first bytes. */
CBMLanguage cbm_disambiguate_res(const char *path) {
    if (!path) {
        return CBM_LANG_RESCRIPT;
    }
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return CBM_LANG_RESCRIPT;
    }
    char buf[CBM_SZ_4K];
    size_t n = fread(buf, SKIP_ONE, sizeof(buf), f);
    (void)fclose(f);
    return memchr(buf, '\0', n) ? CBM_LANG_COUNT : CBM_LANG_RESCRIPT;
}

/* ── .d disambiguation: D source vs make-style dep-info ───────────── */

/* A make rule's targets end at a ':' followed by whitespace or the end of the
 * file, which skips drive-letter colons ("C:/", "C:\"). */
static bool is_rule_colon(const char *p, bool whole_file) {
    char next = p[SKIP_ONE];
    return *p == ':' && (next == ' ' || next == '\t' || next == '\r' || next == '\n' ||
                         (next == '\0' && whole_file));
}

/* Bytes of D whitespace at p, or 0: space, tab, vertical tab, form feed, CR
 * and the UTF-8 line and paragraph separators U+2028/U+2029, all of which split
 * D tokens. '\n' ends the line and is the caller's. */
static size_t d_space_len(const char *p) {
    if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f' || *p == '\r') {
        return SKIP_ONE;
    }
    if (strncmp(p, "\xE2\x80\xA8", SLEN("\xE2\x80\xA8")) == 0 ||
        strncmp(p, "\xE2\x80\xA9", SLEN("\xE2\x80\xA9")) == 0) {
        return SLEN("\xE2\x80\xA8");
    }
    return 0;
}

/* True if the line at p is a make rule "target...: prereq..." whose targets all
 * look like paths (contain '/', '\' or '.'); a backslash-escaped space stays
 * inside its target. D source fails this: "public:", "@safe:", "extern(C):"
 * and "import a.b : c" all have a non-path word or D punctuation before the
 * colon. An attribute ('@', as in "@1.0:") or a comment between tokens
 * ("public/+ +/:") is D even with a '.' or '/' before the colon, so a target
 * holding either is never dep-info. */
static bool is_dep_rule_line(const char *p, bool whole_file) {
    bool in_target = false;
    bool path_like = false;
    bool any_target = false;
    for (; *p && *p != '\n'; p++) {
        if (is_rule_colon(p, whole_file)) {
            return in_target ? path_like : any_target;
        }
        size_t space = d_space_len(p);
        if (space) {
            if (in_target && !path_like) {
                return false;
            }
            in_target = false;
            path_like = false;
            p += space - SKIP_ONE;
            continue;
        }
        if (strchr("(){};=\"',@", *p)) {
            return false;
        }
        if (*p == '/' && (p[SKIP_ONE] == '*' || p[SKIP_ONE] == '+' || p[SKIP_ONE] == '/')) {
            return false;
        }
        in_target = true;
        any_target = true;
        path_like = path_like || *p == '/' || *p == '\\' || *p == '.';
        if (*p == '\\' && p[SKIP_ONE] == ' ') {
            p++; /* escaped space: part of this target */
        }
    }
    return false;
}

/* Disambiguate .d files: shared by D source and make-style dependency files
 * written by rustc/cargo (target/<profile>/deps/<crate>-<hash>.d), gcc/clang -MD
 * and CMake (<object>.o.d). Those are "target: prereq ..." rules, often tens of
 * KB on one line, which the D grammar parses slowly into nothing but a module
 * node, so they are reported as unsupported (CBM_LANG_COUNT). Defaults to D on
 * any doubt (preserves existing behaviour). */
CBMLanguage cbm_disambiguate_d(const char *path) {
    if (!path) {
        return CBM_LANG_DLANG;
    }
    FILE *f = cbm_fopen(path, "r");
    if (!f) {
        return CBM_LANG_DLANG;
    }
    char buf[CBM_SZ_4K + SKIP_ONE];
    size_t n = fread(buf, SKIP_ONE, CBM_SZ_4K, f);
    buf[n] = '\0';
    (void)fclose(f);
    /* A colon at the end of buf only ends the rule if nothing was cut off. */
    bool whole_file = n < CBM_SZ_4K && strlen(buf) == n;

    const char *p = buf;
    if (strncmp(p, "\xEF\xBB\xBF", SLEN("\xEF\xBB\xBF")) == 0) {
        p += SLEN("\xEF\xBB\xBF");
    }
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    /* A shebang or a leading D comment (including the DUB single-file
     * "/+ dub.sdl:" recipe) is D; dep-info never starts that way. */
    if (*p == '#' ||
        (p[0] == '/' && (p[SKIP_ONE] == '/' || p[SKIP_ONE] == '*' || p[SKIP_ONE] == '+'))) {
        return CBM_LANG_DLANG;
    }
    return is_dep_rule_line(p, whole_file) ? CBM_LANG_COUNT : CBM_LANG_DLANG;
}

/* Disambiguate .cls files: shared by Salesforce Apex and Visual Basic 6 class
 * modules (#721). VB6 class modules carry the VB6 header markers and are
 * reported as unsupported (CBM_LANG_COUNT) until a Visual Basic grammar exists;
 * the VB6 class modules otherwise went to the Apex grammar (junk nodes, extract
 * timeouts). Defaults to Apex on any doubt. Upstream also routes ObjectScript
 * UDL here; this fork does not carry ObjectScript. */
CBMLanguage cbm_disambiguate_cls(const char *path) {
    char buf[CBM_SZ_4K + SKIP_ONE];
    if (!path || !read_head_4k(path, buf)) {
        return CBM_LANG_APEX;
    }
    return has_vb6_markers(buf) ? CBM_LANG_COUNT : CBM_LANG_APEX;
}

/* ── .inc file disambiguation ────────────────────────────────────── */

/* A complete leading token, followed by whitespace or punctuation, within a
 * single line. In particular #include_extra is not #include. Returns the text
 * after the token and its trailing whitespace, or NULL. */
static const char *lang_inc_word(const char *line, const char *end, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(end - line) < n || strncmp(line, word, n) != 0 ||
        (line + n < end && (isalnum((unsigned char)line[n]) || line[n] == '_'))) {
        return NULL;
    }
    line += n;
    while (line < end && isspace((unsigned char)*line)) {
        line++;
    }
    return line;
}

/* Recipes can embed C in quoted strings or shell/Python tasks. Recognize the
 * recipe's own syntax before inspecting those bodies as standalone C code. */
static bool lang_inc_recipe_line(const char *line, const char *end) {
    static const char *const words[] = {"require", "include", "inherit", "addtask", "python", NULL};
    for (int i = 0; words[i]; i++) {
        const char *tail = lang_inc_word(line, end, words[i]);
        if (tail && tail > line + strlen(words[i])) {
            return true;
        }
    }
    if ((size_t)(end - line) >= SLEN("do_") && strncmp(line, "do_", SLEN("do_")) == 0) {
        return true;
    }
    const char *p = line;
    while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == ':' || *p == '[' ||
                       *p == ']' || *p == '-' || *p == '.')) {
        p++;
    }
    if (p == line) {
        return false;
    }
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    return p < end && (*p == '=' || ((*p == '?' || *p == '+' || *p == ':' || *p == '.') &&
                                     p + SKIP_ONE < end && p[SKIP_ONE] == '='));
}

/* Skip only leading whitespace and C block comments, keeping block-comment
 * state across lines. Recipe strings and bodies are handled before looking
 * for C. */
static const char *lang_inc_code(const char *line, const char *end, bool *block_comment) {
    while (line < end) {
        if (*block_comment) {
            const char *close = strstr(line, "*/");
            if (!close || close >= end) {
                return end;
            }
            line = close + SLEN("*/");
            *block_comment = false;
            continue;
        }
        if (isspace((unsigned char)*line)) {
            line++;
            continue;
        }
        if ((size_t)(end - line) >= SLEN("/*") && strncmp(line, "/*", SLEN("/*")) == 0) {
            *block_comment = true;
            line += SLEN("/*");
            continue;
        }
        break;
    }
    return line;
}

static bool lang_inc_c_code(const char *line, const char *end) {
    static const char *const words[] = {
        "extern", "static", "typedef", "namespace", "template",
        "struct", "class",  "int",     "void",      NULL,
    };
    for (int i = 0; words[i]; i++) {
        const char *tail = lang_inc_word(line, end, words[i]);
        if (tail && tail < end && *tail != '=') {
            return true;
        }
    }
    const char *tail = lang_inc_word(line, end, "if");
    return tail && tail < end && *tail == '(';
}

/* A '#' line. True when it is an ObjectScript-only directive ("#def1arg",
 * "#;"), which rules C out. Sets *c_family for a C `#include "x"` /
 * `#include <x>`. */
static bool lang_inc_directive(const char *line, const char *end, bool *c_family) {
    line += SKIP_ONE; /* '#' */
    while (line < end && isspace((unsigned char)*line)) {
        line++;
    }
    if (lang_inc_word(line, end, "def1arg") || (line < end && *line == ';')) {
        return true;
    }
    const char *tail = lang_inc_word(line, end, "include");
    if (tail && tail < end && (*tail == '"' || *tail == '<')) {
        *c_family = true;
    }
    return false;
}

/* .inc is shared by C/C++, BitBake and ObjectScript. Select C++ for explicit
 * C-family code (including C fragments); do not infer an enclosing include
 * context. Upstream routes ObjectScript markers (a "ROUTINE <Name>" header,
 * "#def1arg", "#;", a lone #define) to ObjectScript; this fork does not carry
 * ObjectScript, so they keep the BitBake default, as does unknown content. */
static CBMLanguage lang_inc_text(const char *buf) {
    bool c_family = false;
    bool block_comment = false;
    const char *line = buf;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) {
            end = line + strlen(line);
        }
        const char *code = lang_inc_code(line, end, &block_comment);
        line = *end ? end + SKIP_ONE : end;
        if (code == end) {
            continue;
        }
        const char *tail = lang_inc_word(code, end, "ROUTINE");
        if (tail && tail < end && isupper((unsigned char)*tail)) {
            return CBM_LANG_BITBAKE; /* ObjectScript include header: not C */
        }
        if (*code == '#') {
            if (lang_inc_directive(code, end, &c_family)) {
                return CBM_LANG_BITBAKE; /* ObjectScript directive: not C */
            }
            continue;
        }
        if ((size_t)(end - code) >= SLEN("//") && strncmp(code, "//", SLEN("//")) == 0) {
            continue;
        }
        if (!c_family && lang_inc_recipe_line(code, end)) {
            return CBM_LANG_BITBAKE;
        }
        c_family = c_family || lang_inc_c_code(code, end);
    }
    return c_family ? CBM_LANG_CPP : CBM_LANG_BITBAKE;
}

CBMLanguage cbm_disambiguate_inc(const char *path) {
    char buf[CBM_SZ_4K + SKIP_ONE];
    if (!path || !read_head_4k(path, buf)) {
        return CBM_LANG_BITBAKE;
    }
    return lang_inc_text(buf);
}
