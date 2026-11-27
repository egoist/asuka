; The outline: a file's declarations, a type's members, a component's
; props, state and events.

(import_declaration
  "import" @context
  path: (_) @name) @item

(component_declaration
  (visibility)? @context
  "native"? @context
  "component" @context
  name: (_) @name) @item

(screen_declaration
  (visibility)? @context
  "screen" @context
  name: (_) @name) @item

(store_declaration
  (visibility)? @context
  "store" @context
  name: (_) @name) @item

(worker_declaration
  (visibility)? @context
  "worker" @context
  name: (_) @name) @item

(shader_declaration
  (visibility)? @context
  "shader" @context
  name: (_) @name) @item

(struct_declaration
  (visibility)? @context
  "extern"? @context
  "struct" @context
  name: (_) @name) @item

(enum_declaration
  (visibility)? @context
  "enum" @context
  name: (_) @name) @item

(enum_case
  name: (_) @name) @item

(interface_declaration
  (visibility)? @context
  "native"? @context
  "interface" @context
  name: (_) @name) @item

(extend_declaration
  "extend" @context
  type: (_) @name) @item

(type_alias_declaration
  (visibility)? @context
  "type" @context
  name: (_) @name) @item

(extern_type_declaration
  (visibility)? @context
  "extern" @context
  "type" @context
  name: (_) @name) @item

(function_declaration
  (visibility)? @context
  ["static" "extern" "native"]? @context
  "async"? @context
  "mut"? @context
  "fn" @context
  name: (_) @name) @item

(style_declaration
  (visibility)? @context
  "style" @context
  name: (_) @name) @item

(state_declaration
  (visibility)? @context
  "state" @context
  name: (_) @name) @item

(prop_declaration
  (visibility)? @context
  "prop" @context
  name: (_) @name) @item

(event_declaration
  (visibility)? @context
  "event" @context
  name: (_) @name) @item

(field_declaration
  (visibility)? @context
  name: (_) @name) @item

(source_file
  (let_declaration
    (visibility)? @context
    ["let" "var"] @context
    pattern: (binding_pattern name: (_) @name)) @item)

(declaration_body
  (let_declaration
    (visibility)? @context
    ["let" "var"] @context
    pattern: (binding_pattern name: (_) @name)) @item)

(enum_body
  (let_declaration
    (visibility)? @context
    ["let" "var"] @context
    pattern: (binding_pattern name: (_) @name)) @item)

(app_declaration
  "app" @name) @item

(test_declaration
  "test" @context
  name: (_) @name) @item

(bench_declaration
  "bench" @context
  name: (_) @name) @item

(preview_declaration
  "preview" @context
  name: (_) @name) @item
