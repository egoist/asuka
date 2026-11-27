; Asuka's highlights. Later patterns win over earlier ones (Zed, Neovim).

; Names

(identifier) @variable

(parameter name: (identifier) @variable.parameter)
(lambda_parameter name: (identifier) @variable.parameter)
(lambda_expression parameters: (lambda_parameter) @variable.parameter)
(on_handler (parameter) @variable.parameter)

(member_expression property: (identifier) @property)
(field_declaration name: (identifier) @property)
(prop_declaration name: (identifier) @property)
(prop_set name: (identifier) @property)
(bind_item name: (identifier) @property)
(style_application name: (identifier) @property)
(variant_declaration name: (identifier) @property)

(argument label: (identifier) @label)
(field_pattern label: (identifier) @label)
(index_expression label: (identifier) @label)
(enum_case (parameter name: (identifier) @label))
(for_statement label: (identifier) @label)
(while_statement label: (identifier) @label)
(break_expression label: (identifier) @label)
(continue_expression label: (identifier) @label)

; Functions and calls

(function_declaration name: (identifier) @function)
(event_declaration name: (identifier) @function)
(on_handler event: (identifier) @function)
(emit_statement event: (identifier) @function)
(style_declaration name: (identifier) @function)

(call_expression function: (identifier) @function)
(call_expression function: (member_expression property: (identifier) @function.method))
(call_expression function: (generic_expression value: (identifier) @function))
(call_expression
  function: (generic_expression value: (member_expression property: (identifier) @function.method)))

; toolbar { … }, pressed { … }: a named slot or a variant's style.
(call_expression
  function: (identifier) @property
  !arguments
  (#match? @property "^[a-z]"))

; Types

(type_identifier) @type
(type_parameter name: (identifier) @type)

(component_declaration name: (identifier) @type)
(screen_declaration name: (identifier) @type)
(store_declaration name: (identifier) @type)
(worker_declaration name: (identifier) @type)
(shader_declaration name: (identifier) @type)
(struct_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(interface_declaration name: (identifier) @type)
(type_alias_declaration name: (identifier) @type)
(extern_type_declaration name: (identifier) @type)
(use_declaration type: (named_type name: (type_identifier) @type))

; Upper-case names: elements, types, a type's static members.
((identifier) @type
  (#match? @type "^\\p{Lu}"))

((type_identifier) @type.builtin
  (#any-of? @type.builtin
    "int" "float" "bool" "string" "bytes" "char" "void" "never"
    "i8" "i16" "i32" "u8" "u16" "u32" "u64"))

(named_type package: (identifier) @namespace)
(import_path) @namespace
(import_declaration alias: (identifier) @namespace)

; Constants

(enum_case name: (identifier) @constant)
(implicit_member_expression name: (identifier) @constant)
(implicit_case_pattern name: (identifier) @constant)
(case_pattern type: (identifier) name: (identifier) @constant)

(compile_time_constant) @constant.builtin
(none) @constant.builtin
(self) @variable.special
(boolean_literal) @boolean

; Literals

(integer_literal) @number
(float_literal) @number
(duration_literal) @number
(length_literal) @number
(color_literal) @constant

(char_literal) @string
(string_literal) @string
(raw_string_literal) @string
(escape_sequence) @string.escape
(format_specifier) @string.special

(interpolation
  "{" @punctuation.special
  "}" @punctuation.special)

(comment) @comment
((comment) @comment.doc
  (#match? @comment.doc "^///"))

; Keywords

(visibility) @keyword

[
  "app"
  "as"
  "async"
  "at"
  "await"
  "bench"
  "bind"
  "cleanup"
  "component"
  "effect"
  "emit"
  "enum"
  "event"
  "extend"
  "extern"
  "fn"
  "import"
  "in"
  "interface"
  "key"
  "let"
  "load"
  "measure"
  "mut"
  "native"
  "on"
  "preview"
  "prop"
  "provide"
  "release"
  "reload"
  "screen"
  "shader"
  "spawn"
  "state"
  "static"
  "store"
  "struct"
  "style"
  "task"
  "test"
  "throws"
  "type"
  "use"
  "var"
  "variant"
  "with"
  "worker"
] @keyword

[
  "break"
  "catch"
  "continue"
  "defer"
  "do"
  "else"
  "for"
  "if"
  "match"
  "return"
  "throw"
  "try"
  "while"
] @keyword.control

; Operators and punctuation

[
  "!" "!=" "%" "%=" "&&" "&" "&*" "&*=" "&+" "&+=" "&-" "&-=" "&="
  "*" "**" "**=" "*=" "+" "+=" "-" "-=" "->" ".." "..<" "/" "/=" "<"
  "<<" "<<=" "<=" "=" "==" "=>" ">" ">=" "?" "??" "??=" "^" "^=" "|"
  "|=" "||" "~"
] @operator

(unary_expression operator: "await" @keyword)
(try_expression "?" @keyword)

["(" ")" "[" "]" "{" "}" ".{"] @punctuation.bracket

["," ";" ":" "." "?."] @punctuation.delimiter

"@" @constant.builtin
