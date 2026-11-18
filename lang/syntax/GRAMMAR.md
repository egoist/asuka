# The Asuka grammar

The language's syntax as `lang/syntax` parses it: the tokens the
scanner makes (scanner.go) and the productions of the parser
(parser.go), item by item. RFC 0007's grammar was the first; this is the
current one. Where the parser decides what a production leaves open,
parser.go's numbered decisions say how, and this page names them
("decision 4").

Notation: EBNF as RFC 0007 writes it. `=` defines, `|` separates
alternatives, `[ … ]` is optional, `{ … }` repeats zero or more times,
`( … )` groups, `"…"` is a token's text, `.` ends a production and
`(* … *)` is a comment. Upper-case names that the productions don't
define are tokens: `IDENT`, `INT`, `FLOAT`, `DURATION`, `LENGTH`,
`COLOR`, `STRING` (with its interpolations), `NL` (a newline that ends
an item).

## Lexical structure

### Source

A file is UTF-8; a byte order mark at its start is skipped (one anywhere
else is an error). Spaces, tabs and carriage returns separate tokens.

### Comments

```ebnf
Comment     = "//" { any but newline }           (* "///" documents what follows: a doc comment *)
            | "/*" { any } "*/" .                (* not nested *)
```

A comment is kept aside for the printer and the documentation: it's no
token. A block comment that spans lines ends its line as a newline
would.

### Newlines

A newline is a token, `NL`, where it ends an item, a member or a
statement; `;` does the same on a line. A newline is no token:

- inside `( )`, `[ ]` and an import's `.{ }` (inside `{ }` newlines
  count again, and a `}` closes any `(` or `[` left open inside its
  `{`);
- inside a `"""` string's interpolation (a one-line string's
  interpolation ends with its line, and the string is unterminated);
- after a token that needs something on its right: a binary operator,
  `=` and the assignment operators, `=>`, `->`, `.`, `?.`, `..`, `..<`,
  `|`, `,`, `:`, `;`, `else`, an opening bracket;
- after another newline or at the file's start: runs of newlines are
  one;
- before a line (blank lines and comments aside) that starts with a
  binary operator that can't start an expression: `+ * / % == != < <= >
  >= && || ?? & | ^ << >> &+ &- &* .. ..<`, or with `?.`. Not `-`, `!` or `~`,
  which can start one (`a ⏎ -b` is two lines), and not an assignment's
  operator (`+=`).

A newline after `?` (`String?`), `!` (`x!`) or `>` (`Loading<User>`)
counts: a line may end with an optional type, a force unwrap or a
generic type.

A line that starts with `.` and a name is the parser's to join: see
[Line continuation](#line-continuation).

### Names

```ebnf
IDENT       = IdentStart { IdentContinue } .     (* not a keyword *)
IdentStart  = "_" | XID_Start .
IdentContinue = XID_Continue .
```

`XID_Start` and `XID_Continue` are Unicode's (UAX #31's default
identifiers, as Rust, Swift and Python take them): letters of any
script, then letters, combining marks, digits and connector
punctuation (`नमस्ते`, `ราคา`, `名前`, `x١٢`). A name is compared in NFC:
`é` as one code point and `e` with a combining acute are one name (the
parser normalizes each identifier's text).

### Keywords

```
app as async await bind break catch component continue defer do effect
else emit enum event extend extern false fn for if import in interface
key let load match mut native none on prop provide pub return screen
self spawn state store struct style task test throw throws true try type
use var while worker
```

`mut`, `extend` and `worker` are keywords since 0.2.7: a file that
names something so is an error that says to rename it ("rename it:
worker is a keyword since 0.2.7").

After `.` or `?.`, a keyword is a member's name (`x.type`,
`.default`). The soft keywords, which only start a declaration or an
item, are names wherever else a name goes (decision 10):

```ebnf
SoftKeyword = "app" | "bind" | "component" | "effect" | "enum" | "event" | "extern"
            | "interface" | "key" | "native" | "on" | "prop" | "provide" | "screen"
            | "state" | "store" | "struct" | "style" | "task" | "test" | "throws"
            | "type" | "use" .
Ident       = IDENT | SoftKeyword .              (* where a declaration names something *)
Name        = IDENT | Keyword .                  (* a label, a prop set's, a member's, a field's *)
```

`shader`, `at`, `static`, `variant`, `cleanup`, `with` and `release` are
keywords only in their productions below; elsewhere they're names.

### Literals

```ebnf
INT         = Decimal | "0x" HexDigits | "0b" BinDigits | "0o" OctDigits .
Decimal     = digit { [ "_" ] digit } .          (* an _ only between digits *)
FLOAT       = Decimal "." Decimal [ Exponent ] | Decimal Exponent .
Exponent    = ( "e" | "E" ) [ "+" | "-" ] Decimal .
DURATION    = ( Decimal | FLOAT ) ( "ns" | "us" | "ms" | "s" | "min" | "h" ) .
LENGTH      = ( Decimal | FLOAT ) ( "%" | "fr" ) .
COLOR       = "#" hex hex hex [ hex ] | "#" hex hex hex hex hex hex [ hex hex ] .
```

A `%` straight after a number is a length unless what follows it starts
an operand (a letter, a digit, `(`, `[`, `"`, `.`, `#`): `50%` is a
length, `10%3` and `10 % 3` remainders. `1.foo` is `1`, `.`, `foo`.

```ebnf
STRING      = '"' { Char | Escape | Interp } '"'
            | '"""' { any | Escape | Interp } '"""'        (* spans lines *)
            | "r" Hashes '"' { any } '"' Hashes            (* raw: no escapes, no interpolation *)
            | "r" Hashes '"""' { any } '"""' Hashes .
Hashes      = { "#" } .                          (* as many after the string as before it *)
Escape      = "\" ( "n" | "t" | "r" | "0" | '"' | "'" | "\" | "{" | "}" )
            | "\u{" hex [ hex [ hex [ hex [ hex [ hex ] ] ] ] ] "}" .
Interp      = "{" Expr "}" .
```

A one-line string ends at its line's end (unterminated: an error). A
`"""` string holds `"` and `""` as they are; its first line, when
blank, and the indentation of its closing quotes' line aren't part of
it, and its line breaks are `\n` whatever the file's. A raw string
with hashes ends only at a quote followed by as many `#`s:
`r#"say "hi""#`, `r##"a "# b"##`. After the hashes, `"""` opens a
multi-line one.

### Operators and punctuation

```
+ - * / % & | ^ ~ << >> &+ &- &*   == != < <= > >=   && || !   ?? ? ?. .. ..<
= += -= *= /= %= &= |= ^= <<= >>= &+= &-= &*= ??=   => -> . .{ , : ; @
( ) [ ] { }
```

`>>` and `>>=` are two tokens side by side (`>` `>`, `>` `>=`), so a
type's `Box<Box<int>>` closes twice (decision 24). `&+`, `&-` and `&*`
(an int's wrapping arithmetic) are one token each, so `a &-b` is `a &-
b`: `a & -b` needs its space. `++` and `--` are
errors that say what to write.

## Files

```ebnf
File        = { Import Term } { Decl Term } .
Term        = NL | ";" .                         (* or the } or the end of the file that ends the list *)

Import      = "import" ImportPath [ ".{" [ Name { "," Name } [ "," ] ] "}" ] [ "as" Ident ] .
ImportPath  = [ "./" | "../" { "../" } ] Segment { "/" Segment } .
Segment     = [ "@" ] Name { ( "-" | "." ) ( Name | INT ) } .   (* parts side by side: @acme/line-charts; @ only first *)
```

Imports come before the declarations.

## Declarations

```ebnf
Decl        = [ "pub" ] ( Component | Screen | Store | Worker | Struct | Enum | Interface
                        | Func | TypeAlias | StyleDecl | StateDecl | LetDecl
                        | Shader | Native | Extern | App )
            | Extend | Test .

Component   = "component" Ident [ TypeParams ] [ Params ] Block .
Screen      = "screen" Ident [ Params ] [ "at" STRING ] Block .
Store       = "store" Ident [ TypeParams ] [ Params ] Block .
Worker      = "worker" Ident [ Params ] Block .     (* its vars, lets and fns: the checker's *)
Shader      = "shader" Ident [ Params ] "{" { ( Func | Constant ) Term } "}" .
App         = "app" Block .
Test        = "test" STRING Block .

Struct      = "struct" Ident [ TypeParams ] [ Conforms ] "{" { StructMember Term } "}" .
Conforms    = ":" Type { "," Type } .
StructMember = [ "pub" ] ( Func | "static" Func | "native" Func | Constant | Field ) .
Field       = Name ":" Type [ "=" Expr ] .

Enum        = "enum" Ident [ TypeParams ] [ Conforms ]
              "{" { Case ( "," | Term ) } { EnumMember Term } "}" .
Case        = Name [ "(" [ CaseParam { "," CaseParam } [ "," ] ] ")" ] .
CaseParam   = [ Name ":" ] Type .
EnumMember  = [ "pub" ] ( Func | "static" Func | Constant ) .

Interface   = "interface" Ident [ TypeParams ] "{" { InterfaceMember Term } "}" .
Extend      = "extend" [ TypeParams ] Type [ Conforms ] "{" { StructMember Term } "}" .   (* fields: the checker's error *)
InterfaceMember = [ "pub" ] ( Func | "native" Func | Field ) .

Native      = "native" ( Interface | Component | Func ) .
Extern      = "extern" ( Func | Struct | "type" Ident [ "with" "release" Ident ] ) .

Func        = [ "async" ] [ "mut" ] "fn" FuncName [ TypeParams ] Params [ "throws" ] [ "->" Type ]
              [ Block | "=" Expr ] .              (* neither: a signature *)
FuncName    = Ident | Keyword .                  (* a keyword only before ( or <: fn defer() *)
Params      = "(" [ Param { "," Param } [ "," ] ] ")" .
Param       = Name ":" Type [ "=" Expr ] .
TypeParams  = "<" TypeParam { "," TypeParam } [ "," ] ">" .
TypeParam   = Ident [ ":" Type { "&" Type } ] .

TypeAlias   = "type" Ident [ TypeParams ] "=" Type .
StyleDecl   = "style" Ident [ Params ] Block .
StateDecl   = "state" Ident [ Deps ] [ ":" Type ] "=" Expr .
Deps        = "(" Expr { "," Expr } [ "," ] ")" .  (* what it follows; () is an error: peek(…) *)
LetDecl     = ( "let" | "var" ) Pattern [ ":" Type ] "=" Expr [ "else" Block ] .
Constant    = "let" Pattern [ ":" Type ] "=" Expr .   (* a type's: Font.title *)
```

`pub` may precede any declaration or member, and the checker says where
it means something; a test can't be `pub`, nor an `extend` (its members
are, each). `mut fn` is a method's: a struct's, an enum's, an
interface's or an extension's. `var` at a file's top level
is an error (a file's mutable values are `state`). An enum's cases come
before its constants and methods. `let … else`: the `else` is on the
line of the let's value (decision 20), and only a statement's let may
have one (the checker's).

A declaration that can only be a file's (`component`, `screen`,
`store`, `worker`, `struct`, `enum`, `interface`, `extend`, `type`, `native …`, `extern …`,
`import`, `app {`, `test "…"`) in column 1 inside a block ends the
block: its `}` is missing.

## Blocks and items

Every `{ … }` body (a component's, a store's, a worker's, an element's, a
function's, a handler's, a branch's) is parsed by one item grammar;
which items a block may hold is the checker's to say (decision 1).

```ebnf
Block       = "{" { Item Term } "}" .            (* its { on the line of what it belongs to *)

Item        = PropSet
            | [ "pub" ] ( Member | ViewItem | Stmt ) .
PropSet     = Name ":" Expr .                    (* any item that starts with a name and a : *)

Member      = "prop" Ident [ ":" Type ] [ "=" Expr ]
            | "event" Ident [ Params ]
            | StateDecl | LetDecl
            | "use" Ident ":" Type
            | "provide" Expr
            | Func
            | "task" [ "(" [ Expr { "," Expr } [ "," ] ] ")" ] Block
            | "effect" [ "(" [ Expr { "," Expr } [ "," ] ] ")" ] Block
            | "cleanup" Block                      (* cleanup is a name elsewhere (rfcs/0016) *)
            | "variant" Ident "=" Expr             (* variant is a name elsewhere *)
            | "style" Ident [ Params ] Block .     (* a style of the block's own *)

ViewItem    = OnHandler
            | "bind" Name ":" Expr
            | "style" Ident { "." Ident }          (* applies a style: style: name *)
            | Ident Block .                        (* a lower-case name: a named slot, toolbar { … } *)
OnHandler   = "on" Name [ "(" [ Ident { "," Ident } [ "," ] ] ")" ] Block .

Stmt        = IfStmt
            | "for" [ "await" ] Pattern "in" HeadExpr [ "key" HeadExpr ] [ "as" Ident ] Block
            | "while" Cond [ "as" Ident ] Block      (* as names the loop: break rows *)
            | "match" HeadExpr Arms
            | "do" Block CatchClause { CatchClause }
            | "return" [ Expr ]                     (* its value on its line *)
            | "throw" Expr
            | ( "break" | "continue" ) [ Ident ]    (* a loop's name, on break's line *)
            | "defer" Block
            | "cleanup" Block                      (* in an effect's body (rfcs/0016) *)
            | "emit" Name [ Args ]                  (* the ( on emit's line *)
            | Expr [ AssignOp Expr ] .
IfStmt      = "if" Cond Block [ "else" ( IfStmt | Block ) ] .
AssignOp    = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
            | "&+=" | "&-=" | "&*=" | "??=" .

Cond        = CondClause { "," CondClause } .   (* if let a = x, a > 0 { … } (decision 21) *)
CondClause  = ( "let" | "var" ) Pattern "=" Expr | Expr .   (* if var x = y: a copy that may change *)
CatchClause = "catch" [ Ident ":" Type | Pattern ] Block .
Arms        = "{" { Arm ( "," | Term ) } "}" .
Arm         = Pattern [ "if" HeadExpr ] "=>" ( Block | Item ) .   (* a match expression's: Block | Expr *)
```

The grammar's view items and statements share their nodes: an `if`, a
`for` or a `match` among views is the statement's. `else` and `catch`
may start the line after a `}` (decision 6). A soft keyword starts its
member only when what follows fits (`prop` and a name, `app` and `{`,
`test` and a string, …); otherwise it's a name and the item an
expression (`app.tap("Add")`). A block on a line of its own, `Row ⏎ {`,
is an error, as is a declaration that's only a file's.

A `HeadExpr` is an expression in a control clause's head (an `if`'s or
a `while`'s condition, a `for`'s sequence and key, a `match`'s subject,
a guard, a `catch`'s pattern), where a `{` starts the body instead of a
trailing block (decision 4): `if isEmpty { … }`, `for x in Items { … }`.
Inside parentheses or brackets there, trailing blocks bind again.

## Expressions

```ebnf
Expr        = Lambda | Binary [ CatchClause { CatchClause } ] .   (* catch as a value (decision 23) *)
HeadExpr    = Expr .                             (* no trailing blocks, but in brackets *)

Lambda      = [ "async" ] ( Ident | "(" [ LambdaParam { "," LambdaParam } [ "," ] ] ")" )
              "=>" ( Block | Expr ) .
LambdaParam = Name [ ":" Type ] [ "=" Expr ] .

Binary      = Unary { BinaryOp Unary } .         (* by precedence, below *)
BinaryOp    = "??" | "||" | "&&" | "==" | "!=" | "<" | "<=" | ">" | ">=" | ".." | "..<"
            | "+" | "-" | "|" | "^" | "&+" | "&-" | "*" | "/" | "%" | "&" | "&*" | "<<" | ">>" .

Unary       = ( "!" | "-" | "~" | "await" ) Unary
            | "try" [ "?" ] Unary                (* try? : the ? right after try *)
            | "load" Postfix                     (* load (f(x)): a call in parentheses *)
            | "spawn" Block
            | Jump
            | Postfix .
Jump        = "throw" Expr | "return" [ Expr ] | "break" | "continue" .   (* decision 22 *)

Postfix     = Primary { Selector } .
Selector    = "." Name | "?." Name
            | [ TypeArgs ] Args [ "as" Ident ] [ Trailing ]
            | "as" Ident [ Trailing ]            (* after an element's name: View as card { … } *)
            | Trailing                           (* after an element's or a lower-case name *)
            | "[" Expr [ "," "default" ":" Expr ] "]"   (* m[k, default: v]: default is a label only here *)
            | "!" .                              (* right after its operand: x! *)
TypeArgs    = "<" Type { "," Type } [ "," ] ">" .  (* only right before a ( *)
Args        = "(" [ Arg { "," Arg } [ "," ] ] ")" .
Arg         = [ Name ":" ] Expr .
Trailing    = Block { OnHandler } .              (* on the call's line (decision 4, 5) *)

Primary     = Ident | Literal
            | "self"
            | "@" Name                           (* a compile-time constant: @platform, the name right after @ *)
            | "." Name                           (* an implicit member: .gray *)
            | "(" ")"                            (* the empty tuple *)
            | "(" Expr ")"                       (* grouping *)
            | "(" Expr "," [ Expr { "," Expr } [ "," ] ] ")"   (* a tuple: (a,) has one *)
            | "[" [ Expr { "," Expr } [ "," ] ] "]"            (* an array *)
            | "[" ":" "]" | "[" Expr ":" Expr { "," Expr ":" Expr } [ "," ] "]"   (* a map *)
            | IfExpr
            | "match" HeadExpr "{" { Arm ( "," | Term ) } "}" .
IfExpr      = "if" Cond Block [ "else" ( IfExpr | Block ) ] .
Literal     = INT | FLOAT | DURATION | LENGTH | COLOR | STRING | "true" | "false" | "none" .
```

Operator precedence, from the loosest: a lambda, then `catch`; `??`
(right-associative); `||`; `&&`; `== != < <= > >=`; `.. ..<`; `+ - | ^
&+ &-`; `* / % & &*`; `<< >>`; the unary operators; the postfix ones. The binary
operators but `??` are left-associative; comparisons don't chain, nor
do ranges (`a < b < c` is an error that says what to write).

What the parser decides where the productions are ambiguous:

- **Elements, slots and trailing blocks** (decisions 3, 4). An
  upper-case name, or a qualified one whose last part is (`ui.Row`),
  followed by `{` is an element: a call with no parentheses and a
  trailing block. Any call `f(…)` followed by `{` on its line takes it
  as its trailing block: `Text("hi") { color: .red }`. A lower-case
  name followed by `{` at an item's start is a named slot (the checker
  reads one named after a function in a statement as its call, as
  `animate { … }`); elsewhere it's a call with that trailing block
  (`await animate { … }`). A trailing block never binds in a
  `HeadExpr`, and binds only on its call's line.
- **Loops' names**: in a `for`'s or a `while`'s head, `as` names the
  loop, not an element (`for x in Items() as rows {`).
- **Lambdas** (decision 7): a name and `=>`, or a parenthesized list of
  names, each maybe with a type, whose `)` is followed by `=>`, maybe
  after `async`. Anything else in parentheses is a grouping or a tuple.
- **Type arguments**: `f<T>(…)` when what's between `<` and `>` holds
  only what a type has (names, brackets, `?`, `,`, `:`, `fn`, `->`,
  `async`, `throws`, nested `<…>`) and parses as types, and a `(`
  follows the `>`; otherwise `<` is a comparison.
- **if as a value** (decision 12) needs an `else`, but in a `style:`
  prop's value, where an entry may be an `if` without one.
- **peek** isn't syntax: `peek(e)` is a call of a builtin, a name no
  declaration may take (the checker's), as `load` is a form.
- **load** (decision 27): `load` and `(` is a call of a function named
  `load` (`load()`, `load(id)`), unless the parentheses hold one call,
  which is the `load` operator's operand: `load (api.user(id))`.
- **Jumps as values** (decision 22): `throw`, `return`, `break` and
  `continue` where an operand goes are values of type `never`: `x ??
  throw E.missing`. `return` takes a value only when one starts before
  its line's end or what closes its expression. At an item's start
  they're statements.
- **catch as a value** (decision 23): a `catch` after an expression, on
  its line, takes the whole expression before it.

### Line continuation

Besides the newlines the scanner drops (above), a line that starts with
`.` and a name continues the expression on the line before, as a member
of it (decision 26):

```asuka
let names = users
  .filter(u => u.active)
  .map(u => u.name)
```

except where such a line starts something of its own:

- at a match arm's level, where it's the next arm's pattern
  (`.a => x ⏎ .b => y`);
- alone on the last line of a block whose last item is its value (an
  `if` or a `match` used as a value, a closure's, a `catch`'s or a
  `spawn`'s block, and an `if`, a `match` or a `do` inside one), where
  it's that value: `if c { log() ⏎ .red }`. Two `.name` lines or more
  there are a chain still.

## Types

```ebnf
Type        = NonOptType { "?" | "??" } .        (* T?? is an optional optional *)
NonOptType  = IDENT { "." Ident } [ "<" Type { "," Type } [ "," ] ">" ]   (* http.Response<T> *)
            | "[" Type "]"                       (* an array *)
            | "[" Type ":" Type "]"              (* a map *)
            | "(" [ Type { "," Type } [ "," ] ] ")"   (* (T) groups; (T,) and (A, B) are tuples *)
            | [ "async" ] "fn" "(" [ Type { "," Type } [ "," ] ] ")" [ "throws" ] [ "->" Type ] .
```

A function type's result takes the `?` after it: `fn() -> int?`
returns an optional; an optional function is `(fn() -> int)?` (and,
the printer's form for any, `(fn())?`).

## Patterns

```ebnf
Pattern     = PrimaryPattern { "|" PrimaryPattern } .
PrimaryPattern = ( "_" | Ident ) [ ":" Type ]  (* _ or a name binds; with a type, a type test: c: Circle *)
            | Ident { "." Ident } [ CaseArgs ]   (* a case or a struct, when a . or a ( follows the name: Shape.circle(r), Point(x: 0, y) *)
            | "." Name [ CaseArgs ]              (* an implicit case: .ready(v) *)
            | Literal
            | "-" ( INT | FLOAT | DURATION | LENGTH )
            | "(" [ Pattern { "," Pattern } [ "," ] ] ")" .   (* (p) groups; a tuple otherwise *)
CaseArgs    = "(" [ CaseArg { "," CaseArg } [ "," ] ] ")" .
CaseArg     = [ Name ":" ] Pattern .            (* a payload's or a field's label (decision 25) *)
```

A name or `_` with a type after it is a type test (RFC 0011): it
matches a value of an interface, or of a type parameter, when the value
is of that type (`match shape { c: Circle => … }`, `if let c: Circle =
shape`). In a `let`'s pattern the `:` is the let's type instead
(`let c: Circle = shape else { … }` is the type test, said by the
type), and in a `CaseArgs` a `name:` is a label, so a typed name there
takes the label first: `.added(shape: c: Circle)`.
