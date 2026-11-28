package check

import (
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/syntax"
)

// The built-in types' methods: a table from a receiver and a name to a
// signature and the runtime builtin that implements it (core/src/rt's
// lib.c, by name; the lowering calls it with CALLB, the receiver first).
//
// Signatures are written in Asuka's type syntax over the receiver's
// type variables: T an array's element, an optional's or a task's
// value; K and V a map's key and value; U a type the call infers (map's
// result).

// A Method is a built-in type's method.
type Method struct {
	Recv    string // array, map, set, string, bytes, float, int, duration, optional, task, app, loading
	Name    string
	Sig     string // (name: Type, …) -> Result
	Builtin string // the runtime's builtin; "" for one the lowering does itself (Special)
	// Special names what the lowering does instead of a call: len (LEN),
	// enumerated, isNone, isSome, mapGet.
	Special string
	// Inout: changes its receiver (the runtime changes the slot it's
	// passed, copying first when it's shared).
	Inout  bool
	Throws bool
	// Rethrows: the call throws when a closure it's given does (its
	// function parameters say throws; the builtin calls them at once and
	// passes their error on), as Swift's rethrows: xs.map(s => try p(s))
	// is a try.
	Rethrows bool
	// Fails: may raise a runtime error (as an index out of range does),
	// which needs no try.
	Fails bool
	Async bool
	// Order is the builtin's arguments after the receiver, by parameter
	// index, when it isn't the parameters' order.
	Order []int
	// Extra: arguments the lowering appends: "kindU", U's ak_kind;
	// "true" or "false", that bool (one builtin for two methods).
	Extra string
	// Needs: what the receiver's type variable must be: "Equatable",
	// "Comparable", "number", "string".
	Needs string
	// Field: read as a property, without parentheses (g.width,
	// card.hovered); its Sig is "() -> T".
	Field bool
	// Variadic: its last parameter takes any number of arguments, passed
	// to the builtin as an array of them (app.expect("a", "b")).
	Variadic bool
	// Consts: constant ints the lowering appends to the builtin's
	// arguments (the state ui.state reads, where web_navigate goes).
	Consts []int64
	Doc    string

	// Params and Result are parsed from Sig. A signature that names the
	// prelude's types (StrokeStyle, TextFormat) has them Invalid here,
	// and each checker reads it again with its prelude (methodSig).
	Params  []*Param
	Result  Type
	prelude bool
}

// Methods lists every built-in method, the language's reference for
// them (docs/language/types.md lists them too).
var Methods = []*Method{
	// Arrays.
	{Recv: "array", Name: "count", Sig: "() -> int", Special: "len", Doc: "the number of elements"},
	{Recv: "array", Name: "count", Sig: "(where: fn(T) throws -> bool) -> int", Builtin: "arr.count", Rethrows: true, Doc: "the number of elements the predicate is true of"},
	{Recv: "array", Name: "isEmpty", Sig: "() -> bool", Builtin: "arr.is_empty"},
	{Recv: "array", Name: "first", Sig: "() -> T?", Builtin: "arr.first"},
	{Recv: "array", Name: "last", Sig: "() -> T?", Builtin: "arr.last"},
	{Recv: "array", Name: "get", Sig: "(index: int) -> T?", Builtin: "arr.get", Doc: "the element at index, or none when it's out of range"},
	{Recv: "array", Name: "push", Sig: "(value: T)", Builtin: "arr.push", Inout: true},
	{Recv: "array", Name: "insert", Sig: "(value: T, at: int)", Builtin: "arr.insert", Inout: true, Order: []int{1, 0}},
	{Recv: "array", Name: "removeAt", Sig: "(index: int) -> T", Builtin: "arr.remove_at", Inout: true, Fails: true, Doc: "removes and returns the element at index; out of range is an error, as xs[i] is"},
	{Recv: "array", Name: "removeWhere", Sig: "(where: fn(T) throws -> bool) -> int", Builtin: "arr.remove_where", Inout: true, Rethrows: true, Doc: "removes the elements the predicate is true of; how many"},
	{Recv: "array", Name: "pop", Sig: "() -> T?", Builtin: "arr.pop", Inout: true},
	{Recv: "array", Name: "clear", Sig: "()", Builtin: "arr.clear", Inout: true},
	{Recv: "array", Name: "map", Sig: "(transform: fn(T) throws -> U) -> [U]", Builtin: "arr.map", Extra: "kindU", Rethrows: true},
	{Recv: "array", Name: "flatMap", Sig: "(transform: fn(T) throws -> [U]) -> [U]", Builtin: "arr.flat_map", Extra: "kindU", Rethrows: true, Doc: "the arrays transform gives, one after another"},
	{Recv: "array", Name: "compactMap", Sig: "(transform: fn(T) throws -> U?) -> [U]", Builtin: "arr.compact_map", Extra: "kindU", Rethrows: true, Doc: "what transform gives, without its nones: lines.compactMap(l => l.toInt())"},
	{Recv: "array", Name: "filter", Sig: "(where: fn(T) throws -> bool) -> [T]", Builtin: "arr.filter", Rethrows: true},
	{Recv: "array", Name: "reduce", Sig: "(initial: U, combine: fn(U, T) throws -> U) -> U", Builtin: "arr.reduce", Extra: "kindU", Rethrows: true, Doc: "combine of initial and the first element, then of that and the next, and so on: xs.reduce(0, (sum, x) => sum + x)"},
	{Recv: "array", Name: "any", Sig: "(where: fn(T) throws -> bool) -> bool", Builtin: "arr.any", Rethrows: true},
	{Recv: "array", Name: "all", Sig: "(where: fn(T) throws -> bool) -> bool", Builtin: "arr.all", Rethrows: true},
	{Recv: "array", Name: "index", Sig: "(where: fn(T) throws -> bool) -> int?", Builtin: "arr.index", Rethrows: true},
	{Recv: "array", Name: "find", Sig: "(where: fn(T) throws -> bool) -> T?", Builtin: "arr.find", Rethrows: true},
	{Recv: "array", Name: "contains", Sig: "(value: T) -> bool", Builtin: "arr.contains", Needs: "Equatable"},
	{Recv: "array", Name: "sum", Sig: "() -> T", Builtin: "arr.sum", Needs: "number"},
	{Recv: "array", Name: "min", Sig: "() -> T?", Builtin: "arr.min", Needs: "Comparable", Doc: "the least element (the first of equal ones), none when it's empty"},
	{Recv: "array", Name: "min", Sig: "(by: fn(T, T) throws -> bool) -> T?", Builtin: "arr.min", Rethrows: true, Doc: "the least element by a less-than: (a, b) => a.age < b.age"},
	{Recv: "array", Name: "max", Sig: "() -> T?", Builtin: "arr.max", Needs: "Comparable", Doc: "the greatest element (the first of equal ones), none when it's empty"},
	{Recv: "array", Name: "max", Sig: "(by: fn(T, T) throws -> bool) -> T?", Builtin: "arr.max", Rethrows: true, Doc: "the greatest element by a less-than: (a, b) => a.age < b.age"},
	{Recv: "array", Name: "unique", Sig: "() -> [T]", Builtin: "arr.unique", Needs: "Hashable", Doc: "each element once, where it first comes"},
	{Recv: "array", Name: "grouped", Sig: "(by: fn(T) throws -> U) -> [U: [T]]", Builtin: "arr.grouped", Extra: "kindU", Needs: "HashableU", Rethrows: true, Doc: "the elements by the key by gives them, in their order: words.grouped(by: w => w.count())"},
	{Recv: "array", Name: "chunked", Sig: "(size: int) -> [[T]]", Builtin: "arr.chunked", Fails: true, Doc: "the elements in arrays of size, the last holding what's left; a size under 1 is an error"},
	{Recv: "array", Name: "zip", Sig: "(other: [U]) -> [(T, U)]", Special: "zip", Doc: "pairs of this array's and other's elements, as long as the shorter: for (name, age) in names.zip(ages)"},
	{Recv: "array", Name: "reversed", Sig: "() -> [T]", Builtin: "arr.reversed"},
	{Recv: "array", Name: "slice", Sig: "(from: int, to: int) -> [T]", Builtin: "arr.slice", Doc: "the elements from index from up to to, clamped"},
	{Recv: "array", Name: "slice", Sig: "(from: int) -> [T]", Builtin: "arr.slice", Doc: "the elements from index from to the end"},
	{Recv: "array", Name: "prefix", Sig: "(count: int) -> [T]", Builtin: "arr.prefix", Doc: "the first count elements (all of them when there are fewer)"},
	{Recv: "array", Name: "suffix", Sig: "(count: int) -> [T]", Builtin: "arr.suffix", Doc: "the last count elements (all of them when there are fewer)"},
	{Recv: "array", Name: "sorted", Sig: "() -> [T]", Builtin: "arr.sorted", Needs: "Comparable"},
	{Recv: "array", Name: "sorted", Sig: "(by: fn(T, T) throws -> bool) -> [T]", Builtin: "arr.sorted", Rethrows: true, Doc: "sorted by a less-than: (a, b) => a.age < b.age; stable"},
	{Recv: "array", Name: "sort", Sig: "()", Builtin: "arr.sort", Inout: true, Needs: "Comparable"},
	{Recv: "array", Name: "sort", Sig: "(by: fn(T, T) throws -> bool)", Builtin: "arr.sort", Inout: true, Rethrows: true},
	{Recv: "array", Name: "enumerated", Sig: "() -> [(int, T)]", Special: "enumerated", Doc: "the elements with their indexes: for (i, x) in xs.enumerated()"},
	{Recv: "array", Name: "joined", Sig: "(separator: string = \"\") -> string", Builtin: "str.join", Needs: "string"},

	// Maps.
	{Recv: "map", Name: "count", Sig: "() -> int", Special: "len"},
	{Recv: "map", Name: "isEmpty", Sig: "() -> bool", Builtin: "map.is_empty"},
	{Recv: "map", Name: "has", Sig: "(key: K) -> bool", Builtin: "map.has"},
	{Recv: "map", Name: "get", Sig: "(key: K) -> V?", Special: "mapGet"},
	{Recv: "map", Name: "remove", Sig: "(key: K) -> bool", Builtin: "map.delete", Inout: true, Doc: "removes the key; whether it was there"},
	{Recv: "map", Name: "keys", Sig: "() -> [K]", Builtin: "map.keys", Doc: "in insertion order"},
	{Recv: "map", Name: "values", Sig: "() -> [V]", Builtin: "map.values", Doc: "in insertion order"},
	{Recv: "map", Name: "mapValues", Sig: "(transform: fn(V) throws -> U) -> [K: U]", Builtin: "map.map_values", Extra: "kindU", Rethrows: true, Doc: "the same keys, each with what transform makes of its value"},
	{Recv: "map", Name: "filter", Sig: "(where: fn(K, V) throws -> bool) -> [K: V]", Builtin: "map.filter", Rethrows: true, Doc: "the entries the predicate is true of, in their order"},

	// Sets.
	{Recv: "set", Name: "count", Sig: "() -> int", Special: "len", Doc: "the number of elements"},
	{Recv: "set", Name: "isEmpty", Sig: "() -> bool", Builtin: "set.is_empty"},
	{Recv: "set", Name: "contains", Sig: "(value: T) -> bool", Builtin: "set.has", Doc: "whether value is one of the elements"},
	{Recv: "set", Name: "insert", Sig: "(value: T) -> bool", Builtin: "set.insert", Inout: true, Doc: "adds value, last, unless it's an element already; whether it was new"},
	{Recv: "set", Name: "remove", Sig: "(value: T) -> bool", Builtin: "set.remove", Inout: true, Doc: "removes value; whether it was an element"},
	{Recv: "set", Name: "union", Sig: "(other: Set<T>) -> Set<T>", Builtin: "set.union", Doc: "the elements of both: these, then other's new ones"},
	{Recv: "set", Name: "intersection", Sig: "(other: Set<T>) -> Set<T>", Builtin: "set.intersection", Doc: "the elements other has too, in this set's order"},
	{Recv: "set", Name: "subtracting", Sig: "(other: Set<T>) -> Set<T>", Builtin: "set.subtracting", Doc: "the elements other hasn't"},
	{Recv: "set", Name: "isSubset", Sig: "(of: Set<T>) -> bool", Builtin: "set.is_subset", Doc: "whether every element is of's"},
	{Recv: "set", Name: "isSuperset", Sig: "(of: Set<T>) -> bool", Builtin: "set.is_superset", Doc: "whether every element of of's is this set's"},

	// Strings.
	{Recv: "string", Name: "count", Sig: "() -> int", Builtin: "str.len", Doc: "the number of characters (code points)"},
	{Recv: "string", Name: "byteCount", Sig: "() -> int", Builtin: "str.bytes_len", Doc: "the number of bytes (UTF-8)"},
	{Recv: "string", Name: "isEmpty", Sig: "() -> bool", Builtin: "str.is_empty"},
	{Recv: "string", Name: "upper", Sig: "() -> string", Builtin: "unicode.upper", Doc: "in upper case, by Unicode's full mappings in no language's way: \"straße\".upper() is STRASSE"},
	{Recv: "string", Name: "lower", Sig: "() -> string", Builtin: "unicode.lower", Doc: "in lower case, by Unicode's full mappings in no language's way (a word's last Σ is ς)"},
	{Recv: "string", Name: "trim", Sig: "() -> string", Builtin: "str.trim", Doc: "without the white space around it"},
	{Recv: "string", Name: "trimStart", Sig: "() -> string", Builtin: "str.trim_start", Doc: "without the white space before it"},
	{Recv: "string", Name: "trimEnd", Sig: "() -> string", Builtin: "str.trim_end", Doc: "without the white space after it"},
	{Recv: "string", Name: "trimPrefix", Sig: "(prefix: string) -> string", Builtin: "str.trim_prefix", Doc: "without prefix when it starts with it"},
	{Recv: "string", Name: "trimSuffix", Sig: "(suffix: string) -> string", Builtin: "str.trim_suffix", Doc: "without suffix when it ends with it"},
	{Recv: "string", Name: "contains", Sig: "(text: string) -> bool", Builtin: "str.contains", Doc: "whether text is in it"},
	{Recv: "string", Name: "contains", Sig: "(text: string, ignoreCase: bool) -> bool", Builtin: "unicode.contains", Doc: "whether text is in it; ignoreCase: their case foldings compared (foldCase)"},
	{Recv: "string", Name: "hasPrefix", Sig: "(prefix: string) -> bool", Builtin: "str.has_prefix", Doc: "whether it starts with prefix"},
	{Recv: "string", Name: "hasPrefix", Sig: "(prefix: string, ignoreCase: bool) -> bool", Builtin: "unicode.hasPrefix", Doc: "whether it starts with prefix; ignoreCase: their case foldings compared"},
	{Recv: "string", Name: "hasSuffix", Sig: "(suffix: string) -> bool", Builtin: "str.has_suffix", Doc: "whether it ends with suffix"},
	{Recv: "string", Name: "hasSuffix", Sig: "(suffix: string, ignoreCase: bool) -> bool", Builtin: "unicode.hasSuffix", Doc: "whether it ends with suffix; ignoreCase: their case foldings compared"},
	{Recv: "string", Name: "replace", Sig: "(old: string, new: string, count: int = -1) -> string", Builtin: "str.replace", Doc: "old replaced with new: the first count of them, every one when count is negative"},
	{Recv: "string", Name: "split", Sig: "(separator: string, count: int = -1) -> [string]", Builtin: "str.split", Doc: "the parts between separators: at most count, the last holding the rest, every one when count is negative"},
	{Recv: "string", Name: "fields", Sig: "() -> [string]", Builtin: "str.fields", Doc: "the words: the runs of text between white space"},
	{Recv: "string", Name: "lines", Sig: "() -> [string]", Builtin: "str.lines", Doc: "the lines, without their line breaks (\\n or \\r\\n)"},
	{Recv: "string", Name: "find", Sig: "(text: string) -> int?", Builtin: "str.find", Doc: "the byte offset of the first text, or none"},
	{Recv: "string", Name: "findLast", Sig: "(text: string) -> int?", Builtin: "str.find_last", Doc: "the byte offset of the last text, or none"},
	{Recv: "string", Name: "findAny", Sig: "(chars: string) -> int?", Builtin: "str.find_any", Doc: "the byte offset of the first character that's one of chars, or none"},
	{Recv: "string", Name: "slice", Sig: "(from: int, to: int) -> string", Builtin: "str.slice", Doc: "the characters from from up to to, clamped"},
	{Recv: "string", Name: "slice", Sig: "(from: int) -> string", Builtin: "str.slice", Doc: "the characters from from to the end"},
	{Recv: "string", Name: "byteSlice", Sig: "(from: int, to: int) -> string", Builtin: "str.byte_slice", Doc: "the bytes from from up to to (the offsets find gives), clamped"},
	{Recv: "string", Name: "byteSlice", Sig: "(from: int) -> string", Builtin: "str.byte_slice", Doc: "the bytes from from (an offset find gives) to the end"},
	{Recv: "string", Name: "padStart", Sig: "(width: int, with: string = \" \") -> string", Builtin: "str.pad", Extra: "false", Doc: "with copies of with before it, up to width characters: \"7\".padStart(2, with: \"0\") is 07"},
	{Recv: "string", Name: "padEnd", Sig: "(width: int, with: string = \" \") -> string", Builtin: "str.pad", Extra: "true", Doc: "with copies of with after it, up to width characters"},
	{Recv: "string", Name: "toInt", Sig: "() -> int?", Builtin: "str.to_int"},
	{Recv: "string", Name: "toInt", Sig: "(radix: int) -> int?", Builtin: "str.to_int_radix", Fails: true, Doc: "the whole string as an int in radix (2 to 36; letters either case): \"ff\".toInt(radix: 16); none when it isn't one"},
	{Recv: "string", Name: "toFloat", Sig: "() -> float?", Builtin: "str.to_float"},
	{Recv: "string", Name: "repeat", Sig: "(count: int) -> string", Builtin: "str.repeat"},
	{Recv: "string", Name: "chars", Sig: "() -> [char]", Builtin: "str.codepoints", Doc: "its chars, the code points for goes over (count() of them), in order"},
	{Recv: "char", Name: "code", Sig: "() -> int", Builtin: "char.of", Doc: "its code point: 'A'.code() is 65"},
	{Recv: "char", Name: "isLetter", Sig: "() -> bool", Builtin: "char.is_letter", Doc: "whether it's a letter of any script (Unicode's L categories): a, Ж, 字"},
	{Recv: "char", Name: "isDigit", Sig: "() -> bool", Builtin: "char.is_digit", Doc: "whether it's a decimal digit of any script (Nd): 0 to 9, ٣"},
	{Recv: "char", Name: "isNumber", Sig: "() -> bool", Builtin: "char.is_number", Doc: "whether it's a number (N): a digit, a Roman numeral Ⅻ, a fraction ½"},
	{Recv: "char", Name: "isWhitespace", Sig: "() -> bool", Builtin: "char.is_whitespace", Doc: "whether it's white space (Unicode's White_Space): a space, a tab, a line ending, a no-break space"},
	{Recv: "char", Name: "isUpper", Sig: "() -> bool", Builtin: "char.is_upper", Doc: "whether it's an upper-case (or title-case) letter"},
	{Recv: "char", Name: "isLower", Sig: "() -> bool", Builtin: "char.is_lower", Doc: "whether it's a lower-case letter"},
	{Recv: "char", Name: "isPunctuation", Sig: "() -> bool", Builtin: "char.is_punctuation", Doc: "whether it's punctuation (P): . , ! ? 「」"},
	{Recv: "char", Name: "isSymbol", Sig: "() -> bool", Builtin: "char.is_symbol", Doc: "whether it's a symbol (S): + $ © and emoji"},
	{Recv: "char", Name: "upper", Sig: "() -> char", Builtin: "char.upper", Doc: "its upper case, where that's one char ('ß' stays 'ß': its upper case, SS, is a string's: \"ß\".upper())"},
	{Recv: "char", Name: "lower", Sig: "() -> char", Builtin: "char.lower", Doc: "its lower case, where that's one char"},
	{Recv: "string", Name: "graphemes", Sig: "() -> [string]", Builtin: "unicode.graphemes", Doc: "its grapheme clusters (Unicode's UAX #29), each a string: what a reader takes for one character, an e and its accent, a flag, a family emoji"},
	{Recv: "string", Name: "foldCase", Sig: "() -> string", Builtin: "unicode.foldCase", Doc: "its case folding (Unicode's full one, in no language's way): what compares strings without their case, as a map's key; \"Straße\" and \"STRASSE\" both fold to strasse"},
	{Recv: "string", Name: "equals", Sig: "(other: string) -> bool", Builtin: "str.equals", Doc: "whether it's other (==)"},
	{Recv: "string", Name: "equals", Sig: "(other: string, ignoreCase: bool) -> bool", Builtin: "unicode.equals", Doc: "whether it's other (==); ignoreCase: their case foldings compared, as Go's strings.EqualFold: \"Go\".equals(\"GO\", ignoreCase: true)"},
	{Recv: "string", Name: "compare", Sig: "(other: string) -> int", Builtin: "str.compare", Doc: "negative, zero or positive as it orders before other, with it or after it: by code point (as <)"},
	{Recv: "string", Name: "compare", Sig: "(other: string, ignoreCase: bool) -> int", Builtin: "unicode.compare", Doc: "negative, zero or positive as it orders before other, with it or after it: by their case foldings with ignoreCase: names.sorted(by: (a, b) => a.compare(b, ignoreCase: true) < 0)"},
	{Recv: "string", Name: "reversed", Sig: "() -> string", Builtin: "str.reversed", Doc: "its characters (code points) in the other order"},
	{Recv: "string", Name: "bytes", Sig: "() -> bytes", Builtin: "str.to_bytes", Doc: "its UTF-8 bytes"},

	// Colors.
	{Recv: "color", Name: "hex", Sig: "() -> string", Builtin: "color.hex", Doc: "the color as it's written: #rrggbb (#rrggbbaa when it isn't opaque), a semantic color by its name (.accent); a color in a string, \"{c}\", is the same"},
	{Recv: "color", Name: "opacity", Sig: "(alpha: float) -> Color", Builtin: "color.opacity", Doc: "the color drawn at alpha (0 to 1) times its own opacity; a semantic color stays one, so it still follows dark mode"},

	// Bytes.
	{Recv: "bytes", Name: "count", Sig: "() -> int", Builtin: "bytes.len", Doc: "the number of bytes"},
	{Recv: "bytes", Name: "isEmpty", Sig: "() -> bool", Builtin: "bytes.is_empty"},
	{Recv: "bytes", Name: "get", Sig: "(index: int) -> int?", Builtin: "bytes.get", Doc: "the byte at index (0 to 255), or none when it's out of range"},
	{Recv: "bytes", Name: "slice", Sig: "(from: int, to: int) -> bytes", Builtin: "bytes.slice", Doc: "the bytes from from up to to, clamped"},
	{Recv: "bytes", Name: "slice", Sig: "(from: int) -> bytes", Builtin: "bytes.slice", Doc: "the bytes from from to the end"},
	{Recv: "bytes", Name: "concat", Sig: "(other: bytes) -> bytes", Builtin: "bytes.concat", Doc: "these bytes, then other's"},
	{Recv: "bytes", Name: "toArray", Sig: "() -> [u8]", Builtin: "bytes.to_array", Doc: "the bytes as an array of u8s; bytes(xs) makes them back"},
	{Recv: "bytes", Name: "string", Sig: "() -> string", Builtin: "bytes.str", Doc: "the bytes read as UTF-8 text; what isn't UTF-8 becomes U+FFFD"},
	{Recv: "bytes", Name: "hex", Sig: "() -> string", Builtin: "bytes.hex", Doc: "the bytes in lowercase hexadecimal: \"04a1\""},
	{Recv: "bytes", Name: "hex", Sig: "(separator: string) -> string", Builtin: "bytes.hex", Doc: "the bytes in lowercase hexadecimal, separator between them: \"04 a1\""},

	// Numbers.
	{Recv: "int", Name: "float", Sig: "() -> float", Special: "i2f", Doc: "the int as a float, as float(n)"},
	{Recv: "int", Name: "abs", Sig: "() -> int", Builtin: "math.absi", Fails: true, Doc: "the int without its sign; the smallest int's would overflow, which throws"},
	{Recv: "int", Name: "toString", Sig: "(radix: int) -> string", Builtin: "int.to_string", Fails: true, Doc: "in radix (2 to 36, lowercase letters), with a - when it's negative: 255.toString(radix: 16) is ff"},
	{Recv: "int", Name: "padded", Sig: "(width: int) -> string", Builtin: "int.padded", Doc: "in decimal, zeros before it up to width digits: 7.padded(2) is 07"},
	{Recv: "int", Name: "addingChecked", Sig: "(o: int) -> int?", Builtin: "int.add_checked", Doc: "the sum, or none when it overflows: as +, which throws then"},
	{Recv: "int", Name: "subtractingChecked", Sig: "(o: int) -> int?", Builtin: "int.sub_checked", Doc: "the difference, or none when it overflows: as -, which throws then"},
	{Recv: "int", Name: "multipliedChecked", Sig: "(o: int) -> int?", Builtin: "int.mul_checked", Doc: "the product, or none when it overflows: as *, which throws then"},
	// The sized integers (i8 to u64): T is the receiver's type, and
	// "intcode" appends its code (IntCode), which the builtins read it by.
	{Recv: "sized", Name: "float", Sig: "() -> float", Special: "i2f", Doc: "the integer as a float, as float(n)"},
	{Recv: "sized", Name: "abs", Sig: "() -> T", Builtin: "int.abs", Needs: "signed", Extra: "intcode", Fails: true, Doc: "the integer without its sign; the smallest one's would overflow, which throws"},
	{Recv: "sized", Name: "toString", Sig: "(radix: int) -> string", Builtin: "int.to_string", Extra: "intcode", Fails: true, Doc: "in radix (2 to 36, lowercase letters), with a - when it's negative"},
	{Recv: "sized", Name: "padded", Sig: "(width: int) -> string", Builtin: "int.padded", Extra: "intcode", Doc: "in decimal, zeros before it up to width digits"},
	{Recv: "sized", Name: "addingChecked", Sig: "(o: T) -> T?", Builtin: "int.add_checked", Extra: "intcode", Doc: "the sum, or none when it overflows: as +, which throws then"},
	{Recv: "sized", Name: "subtractingChecked", Sig: "(o: T) -> T?", Builtin: "int.sub_checked", Extra: "intcode", Doc: "the difference, or none when it overflows: as -, which throws then"},
	{Recv: "sized", Name: "multipliedChecked", Sig: "(o: T) -> T?", Builtin: "int.mul_checked", Extra: "intcode", Doc: "the product, or none when it overflows: as *, which throws then"},
	{Recv: "float", Name: "int", Sig: "() -> int", Special: "f2i", Doc: "toward zero, as int(x): x.rounded().int() is the nearest"},
	{Recv: "float", Name: "abs", Sig: "() -> float", Builtin: "math.absf"},
	{Recv: "float", Name: "fixed", Sig: "(digits: int) -> string", Builtin: "str.fixed", Doc: "with that many digits after the point: 3.14159.fixed(2) is 3.14"},
	{Recv: "float", Name: "rounded", Sig: "() -> float", Builtin: "math.round"},
	{Recv: "float", Name: "floor", Sig: "() -> float", Builtin: "math.floor"},
	{Recv: "float", Name: "ceil", Sig: "() -> float", Builtin: "math.ceil"},

	// Durations.
	{Recv: "duration", Name: "nanoseconds", Sig: "() -> int", Builtin: "dur.ns"},
	{Recv: "duration", Name: "milliseconds", Sig: "() -> int", Builtin: "dur.ms", Doc: "whole milliseconds, toward zero"},
	{Recv: "duration", Name: "seconds", Sig: "() -> float", Builtin: "dur.seconds", Doc: "how many seconds: 1500ms.seconds() is 1.5"},
	{Recv: "duration", Name: "minutes", Sig: "() -> float", Builtin: "dur.minutes"},
	{Recv: "duration", Name: "hours", Sig: "() -> float", Builtin: "dur.hours"},
	{Recv: "duration", Name: "rounded", Sig: "(to: Duration) -> Duration", Builtin: "dur.rounded", Doc: "to the nearest multiple of to, halves away from zero: (90s).rounded(to: 1min) is 2min"},
	{Recv: "duration", Name: "truncated", Sig: "(to: Duration) -> Duration", Builtin: "dur.truncated", Doc: "to a multiple of to, toward zero"},

	// Optionals.
	{Recv: "optional", Name: "isNone", Sig: "() -> bool", Special: "isNone"},
	{Recv: "optional", Name: "isSome", Sig: "() -> bool", Special: "isSome"},

	// What mount returns, in tests.
	{Recv: "app", Name: "tap", Sig: "(text: string)", Builtin: "test.tap", Doc: "taps the control showing text, and waits for what it does"},
	{Recv: "app", Name: "type", Sig: "(text: string, into: string)", Builtin: "test.type", Doc: "types text into the field labelled or showing into: a TextField, an alert's field, or a screen's search field by its placeholder"},
	{Recv: "app", Name: "expect", Sig: "(text: string)", Builtin: "test.expect", Variadic: true, Doc: "fails the test unless each text is shown: a text, a title, a field's text or placeholder, an accessibility label, a tab, a bar button, an alert's button, an open menu's item"},
	{Recv: "app", Name: "expectNo", Sig: "(text: string)", Builtin: "test.expect_no", Variadic: true, Doc: "fails the test if any of the texts is shown"},
	{Recv: "app", Name: "has", Sig: "(text: string) -> bool", Builtin: "test.has", Doc: "whether the screen shows text, anywhere"},
	{Recv: "app", Name: "texts", Sig: "() -> [string]", Builtin: "test.texts", Doc: "the texts the screen shows, in the order a screen reader reads them"},
	{Recv: "app", Name: "snapshot", Sig: "() -> string", Builtin: "test.dump", Doc: "what the screen shows: each layer's views, a line each, with their kinds, frames, texts and states"},
	{Recv: "app", Name: "disabled", Sig: "(text: string) -> bool", Builtin: "test.disabled", Doc: "whether what shows text is disabled: a control (or a view around it), a bar button, an alert's button, an open menu's item; it fails unless one thing shows text"},
	{Recv: "app", Name: "longPress", Sig: "(text: string)", Builtin: "test.long_press", Doc: "long-presses what shows text: a view that takes a long press, or one with a context menu, which opens"},
	{Recv: "app", Name: "submit", Sig: "(label: string = \"\")", Builtin: "test.submit", Doc: "presses return in the field label names (as type finds it; \"\": the field typed in last), an alert's field, or a screen's search field (its searchSubmit)"},
	{Recv: "app", Name: "toggle", Sig: "(label: string)", Builtin: "test.toggle", Doc: "flips the switch or checkbox label names: one with that accessibility label, or the one next to a text"},
	{Recv: "app", Name: "slide", Sig: "(label: string, value: float)", Builtin: "test.slide", Doc: "moves the slider label names (as toggle finds it) to value"},
	{Recv: "app", Name: "select", Sig: "(option: string)", Builtin: "test.select", Doc: "chooses option in the picker, wheel or segmented control that has it"},
	{Recv: "app", Name: "pickColor", Sig: "(label: string, color: Color)", Builtin: "test.pick_color", Doc: "chooses color in the ColorPicker label names (as toggle finds it)"},
	{Recv: "app", Name: "pickDate", Sig: "(label: string, value: int)", Builtin: "test.pick_date", Doc: "chooses the instant value (milliseconds since 1970) in the DatePicker label names (as toggle finds it)"},
	{Recv: "app", Name: "pickRange", Sig: "(label: string, start: int, end: int)", Builtin: "test.pick_range", Doc: "chooses the days start to end (milliseconds since 1970: their midnights) in the DateRangePicker label names (as toggle finds it)"},
	{Recv: "app", Name: "remove", Sig: "(text: string)", Builtin: "test.remove", Doc: "presses the remove button of the input chip showing text (a Chip with on remove)"},
	{Recv: "app", Name: "chooseSuggestion", Sig: "(text: string)", Builtin: "test.choose_suggestion", Doc: "chooses the suggestion text in the field that lists it now (a TextField's suggestions, as what's typed filters them)"},
	{Recv: "app", Name: "doubleTap", Sig: "(text: string)", Builtin: "test.double_tap", Doc: "double-taps what shows text: a view that takes a double tap (on doubleTap), or a zoomable Scroll, which zooms in, or back out"},
	{Recv: "app", Name: "zoom", Sig: "(text: string, scale: float)", Builtin: "test.zoom", Doc: "pinches the zoomable Scroll showing text to scale (it settles within its minZoom and maxZoom)"},
	{Recv: "app", Name: "moveRow", Sig: "(text: string, to: int)", Builtin: "test.move_row", Doc: "drags the list row showing text to index to, as the user moves a row of a List with on move"},
	{Recv: "app", Name: "swipe", Sig: "(text: string, action: string)", Builtin: "test.swipe", Doc: "swipes the list row showing text and presses its swipe action titled action (a trailing one, or a leading one)"},
	{Recv: "app", Name: "back", Sig: "()", Builtin: "test.back", Doc: "goes back as the user's back gesture or button does: an open menu closes; else the top screen of the deepest stack pops; else the top sheet, alert or popover is dismissed (onBack can keep it); else, on Android, the root takes it if it intercepts it"},
	{Recv: "app", Name: "dismiss", Sig: "()", Builtin: "test.dismiss", Doc: "dismisses what's on top as the user does (a swipe down, a tap outside): an open menu, else the top alert, sheet or popover"},
	{Recv: "app", Name: "refresh", Sig: "(text: string)", Builtin: "test.refresh", Doc: "pulls the scroll view or list showing text to refresh it"},
	{Recv: "app", Name: "scrollTo", Sig: "(text: string)", Builtin: "test.scroll_to", Doc: "scrolls the lists on the screen until one shows text, as a user looking for it would"},
	{Recv: "app", Name: "until", Sig: "(what: string, cond: fn() -> bool)", Builtin: "test.until", Async: true, Doc: "waits until cond holds, as idle does (time moves to each timer as it comes due); it fails, saying what it waited for, once nothing is left to run and cond still doesn't"},
	{Recv: "app", Name: "waitFor", Sig: "(text: string)", Builtin: "test.wait_for", Async: true, Doc: "waits until the screen shows text (as until)"},
	{Recv: "app", Name: "waitGone", Sig: "(text: string)", Builtin: "test.wait_gone", Async: true, Doc: "waits until the screen no longer shows text (as until)"},
	{Recv: "app", Name: "href", Sig: "() -> string", Builtin: "test.href", Field: true, Doc: "the address the main window's Router shows (\"\" with no Router)"},
	{Recv: "app", Name: "history", Sig: "() -> ([string], int)", Builtin: "test.history", Doc: "the main window's history, as a browser keeps it from its Router's addresses: their hrefs, oldest first, and the index of the one shown"},
	{Recv: "app", Name: "goBack", Sig: "()", Builtin: "test.go_back", Doc: "goes back in the main window's history, as a browser's back button does"},
	{Recv: "app", Name: "goForward", Sig: "()", Builtin: "test.go_forward", Doc: "goes forward in the main window's history, as a browser's forward button does"},
	{Recv: "app", Name: "in", Sig: "(scope: TestScope) -> TestApp", Builtin: "test.in", Doc: "the app narrowed to a part of the screen: what it finds and taps is there (app.in(.bottomToolbar).tap(\"Star\"))"},
	{Recv: "app", Name: "nextTo", Sig: "(label: string) -> TestApp", Builtin: "test.next_to", Doc: "the app narrowed to what's next to the view showing label, as a user reads a row or a form (app.nextTo(\"Ada\").tap(\"Remove\"))"},
	{Recv: "app", Name: "frameOf", Sig: "(text: string) -> (float, float, float, float)", Builtin: "test.frame_of", Doc: "the frame (x, y, width, height) in the window of the view showing text"},
	{Recv: "app", Name: "setLanguages", Sig: "(languages: string)", Builtin: "test.set_languages", Doc: "says the user's preferred languages changed (BCP 47 tags, most preferred first, joined by commas)"},
	{Recv: "app", Name: "idle", Sig: "()", Builtin: "test.idle", Async: true, Doc: "waits until nothing is left to run"},

	{Recv: "app", Name: "ops", Sig: "() -> string", Builtin: "test.ops", Doc: "the ops the host applied since the last ops(), a line each (the fake host's log)"},
	{Recv: "app", Name: "press", Sig: "(text: string, down: bool = true)", Builtin: "test.press", Doc: "puts a finger down on the view showing text (or the view around it that reports pressed), or lifts it: its pressed state"},
	{Recv: "app", Name: "hover", Sig: "(text: string, over: bool = true)", Builtin: "test.hover", Doc: "moves the pointer over the view showing text, or off it"},
	{Recv: "app", Name: "frame", Sig: "(time: Duration)", Builtin: "test.frame", Doc: "draws a frame at time: what onFrame registered runs"},
	{Recv: "app", Name: "lifecycle", Sig: "(state: Lifecycle)", Builtin: "test.lifecycle", Doc: "the app comes to the foreground, goes to the background, or the system runs low on memory"},
	{Recv: "app", Name: "openURL", Sig: "(url: string)", Builtin: "test.open_url", Doc: "opens the app with url, as another app does"},
	{Recv: "app", Name: "share", Sig: "(items: [SharedItem])", Builtin: "test.share", Doc: "another app shares items with the app, from its share sheet: onShare (or the app's on share) hears them"},
	{Recv: "app", Name: "tapLink", Sig: "(url: string)", Builtin: "test.link", Doc: "taps the link to url in the rich text that has one"},
	{Recv: "app", Name: "emit", Sig: "(view: string, event: string, value: string = \"\")", Builtin: "test.emit", Doc: "sends a native component's event: from the view of that kind (\"<package>#Name\"), its value v an encoded string"},

	// The desktop, as the fake host keeps it: windows, the menu bar, the
	// tray and the Dock menu, quitting.
	{Recv: "app", Name: "windows", Sig: "() -> string", Builtin: "test.windows", Doc: "the windows open, a line each: the main one first, then each Window by its title, with its options and state"},
	{Recv: "app", Name: "closeWindow", Sig: "(title: string = \"\")", Builtin: "test.close_window", Doc: "the user closes the window titled title (\"\": the main window): its close button"},
	{Recv: "app", Name: "windowActions", Sig: "(title: string = \"\") -> string", Builtin: "test.window_actions", Doc: "the WindowAction commands the window titled title (\"\": the main one) got since the last call, separated by spaces"},
	{Recv: "app", Name: "setWindowState", Sig: "(title: string, state: WindowState)", Builtin: "test.set_window_state", Doc: "the user moves, resizes, minimizes or focuses the window titled title (\"\": the main one)"},
	{Recv: "app", Name: "focusWindow", Sig: "(title: string = \"\")", Builtin: "test.focus_window", Doc: "the user makes the window titled title the one in use (\"\": the main one): where keys go"},
	{Recv: "app", Name: "pressKey", Sig: "(key: string, primary: bool = false, shift: bool = false, alt: bool = false, control: bool = false) -> bool", Builtin: "test.press_key", Doc: "presses a key: to the view with the keyboard's focus and out from it, else the window in use's. Whether a view's or the window's keys took it; one they didn't does what a desktop's does (Tab moves the focus, Enter and Space press the focused view, a character is its keyText)"},
	{Recv: "app", Name: "tab", Sig: "(backward: bool = false)", Builtin: "test.tab", Doc: "moves the keyboard's focus as Tab does (Shift+Tab: backward), within the focus trap it's in"},
	{Recv: "app", Name: "focused", Sig: "() -> string", Builtin: "test.focused", Doc: "what the view with the keyboard's focus shows (its text, or the first text in it): \"\" when nothing has it"},
	{Recv: "app", Name: "menuBar", Sig: "() -> string", Builtin: "test.menu_bar", Doc: "the menu bar's items, a line each: Menu > Item, with its shortcut and state"},
	{Recv: "app", Name: "chooseMenuItem", Sig: "(path: string)", Builtin: "test.choose_menu_item", Doc: "chooses the menu bar's item at path (\"File > Export > PDF\"), as a click or its shortcut does"},
	{Recv: "app", Name: "trays", Sig: "() -> string", Builtin: "test.trays", Doc: "the tray icons shown, a line each with its menu's items under it"},
	{Recv: "app", Name: "clickTray", Sig: "(id: string)", Builtin: "test.click_tray", Doc: "clicks the tray icon id"},
	{Recv: "app", Name: "chooseTrayItem", Sig: "(id: string, path: string)", Builtin: "test.choose_tray_item", Doc: "chooses the item at path (\"Length > 50 minutes\") of tray icon id's menu (\"\": the Dock menu's)"},
	{Recv: "app", Name: "dockMenu", Sig: "() -> string", Builtin: "test.dock_menu", Doc: "the Dock menu's items, a line each"},
	{Recv: "app", Name: "requestQuit", Sig: "(sessionEnding: bool = false) -> bool", Builtin: "test.request_quit", Doc: "the user quits the app (⌘Q): whether the app let it"},
	{Recv: "app", Name: "quits", Sig: "() -> string", Builtin: "test.quits", Doc: "the quits the app asked the host for (quit, relaunch), separated by spaces"},
	{Recv: "app", Name: "secondInstance", Sig: "(args: [string], dir: string)", Builtin: "test.second_instance", Doc: "the app is started again while it runs"},

	// Loading<T>.
	{Recv: "loading", Name: "reload", Sig: "()", Builtin: "load.reload", Doc: "runs the load again: it holds what it shows, loading true until it answers"},
	{Recv: "loading", Name: "optimistic", Sig: "(change: fn(T) -> T)", Builtin: "load.optimistic", Doc: "shows change applied to what the load shows, at once, until its next answer, a throw out of the task that made it, or that task's end with no run to confirm it (change is a value function)"},

	// Graphics: a Canvas's, while its draw runs (core/src/ui/canvas.c).
	{Recv: "graphics", Name: "width", Sig: "() -> float", Builtin: "canvas.width", Field: true, Doc: "the canvas's width, in points"},
	{Recv: "graphics", Name: "height", Sig: "() -> float", Builtin: "canvas.height", Field: true},
	{Recv: "graphics", Name: "path", Sig: "() -> Path", Builtin: "canvas.path", Doc: "an empty path of the canvas's own, reused from draw to draw (drawing again allocates nothing)"},
	{Recv: "graphics", Name: "fill", Sig: "(path: Path, color: Color)", Builtin: "canvas.fill"},
	{Recv: "graphics", Name: "fill", Sig: "(path: Path, gradient: LinearGradient)", Builtin: "canvas.fill_gradient"},
	{Recv: "graphics", Name: "fill", Sig: "(path: Path, gradient: RadialGradient)", Builtin: "canvas.fill_radial"},
	{Recv: "graphics", Name: "stroke", Sig: "(path: Path, color: Color, style: StrokeStyle? = none)", Builtin: "canvas.stroke"},
	{Recv: "graphics", Name: "stroke", Sig: "(path: Path, gradient: LinearGradient, style: StrokeStyle? = none)", Builtin: "canvas.stroke_gradient", Order: []int{0, 2, 1}},
	{Recv: "graphics", Name: "stroke", Sig: "(path: Path, gradient: RadialGradient, style: StrokeStyle? = none)", Builtin: "canvas.stroke_radial", Order: []int{0, 2, 1}},
	{Recv: "graphics", Name: "save", Sig: "()", Builtin: "canvas.save", Doc: "keeps the transform, the clip, the opacity and the fill rule, for restore"},
	{Recv: "graphics", Name: "restore", Sig: "()", Builtin: "canvas.restore"},
	{Recv: "graphics", Name: "translate", Sig: "(x: float, y: float)", Builtin: "canvas.translate"},
	{Recv: "graphics", Name: "scale", Sig: "(x: float, y: float)", Builtin: "canvas.scale"},
	{Recv: "graphics", Name: "rotate", Sig: "(degrees: float)", Builtin: "canvas.rotate", Doc: "clockwise"},
	{Recv: "graphics", Name: "opacity", Sig: "(alpha: float)", Builtin: "canvas.opacity"},
	{Recv: "graphics", Name: "clip", Sig: "(path: Path)", Builtin: "canvas.clip"},
	{Recv: "graphics", Name: "transform", Sig: "(a: float, b: float, c: float, d: float, tx: float, ty: float)", Builtin: "canvas.transform"},
	{Recv: "graphics", Name: "evenOdd", Sig: "(on: bool)", Builtin: "canvas.even_odd", Doc: "fills by the even-odd rule (holes), not the nonzero one"},
	{Recv: "graphics", Name: "text", Sig: "(text: string, x: float, y: float, style: CanvasText? = none)", Builtin: "canvas.text", Doc: "on one line, its baseline at y"},

	// Path: an outline, built by its methods; each returns it, to chain.
	{Recv: "path", Name: "move", Sig: "(x: float, y: float) -> Path", Builtin: "path.move"},
	{Recv: "path", Name: "line", Sig: "(x: float, y: float) -> Path", Builtin: "path.line"},
	{Recv: "path", Name: "quad", Sig: "(cx: float, cy: float, x: float, y: float) -> Path", Builtin: "path.quad"},
	{Recv: "path", Name: "cubic", Sig: "(c1x: float, c1y: float, c2x: float, c2y: float, x: float, y: float) -> Path", Builtin: "path.cubic"},
	{Recv: "path", Name: "rect", Sig: "(x: float, y: float, width: float, height: float) -> Path", Builtin: "path.rect"},
	{Recv: "path", Name: "roundedRect", Sig: "(x: float, y: float, width: float, height: float, radius: float) -> Path", Builtin: "path.rounded_rect"},
	{Recv: "path", Name: "ellipse", Sig: "(x: float, y: float, width: float, height: float) -> Path", Builtin: "path.ellipse", Doc: "the ellipse in the rectangle"},
	{Recv: "path", Name: "arc", Sig: "(cx: float, cy: float, radius: float, start: float, end: float, clockwise: bool = true) -> Path", Builtin: "path.arc", Doc: "radians, from the positive x axis, clockwise as the screen turns"},
	{Recv: "path", Name: "close", Sig: "() -> Path", Builtin: "path.close"},

	// Ref: a view's handle. Each reads the view it holds; with none (the
	// view isn't shown), a command does nothing and a state is false.
	{Recv: "ref", Name: "hovered", Sig: "() -> bool", Builtin: "ui.state", Field: true, Consts: []int64{1}, Doc: "a pointer is over the view (tracked)"},
	{Recv: "ref", Name: "pressed", Sig: "() -> bool", Builtin: "ui.state", Field: true, Consts: []int64{2}, Doc: "a finger or the mouse's button is down on it (tracked)"},
	{Recv: "ref", Name: "dragging", Sig: "() -> bool", Builtin: "ui.state", Field: true, Consts: []int64{4}, Doc: "it's being dragged (tracked)"},
	{Recv: "ref", Name: "dragOver", Sig: "() -> bool", Builtin: "ui.state", Field: true, Consts: []int64{8}, Doc: "a drag it takes is over it (tracked)"},
	{Recv: "ref", Name: "id", Sig: "() -> int", Builtin: "ui.view_id", Field: true, Doc: "the view's identity: the same while it shows, no other view's while it does (0: no view yet)"},
	{Recv: "ref", Name: "focusVisible", Sig: "() -> bool", Builtin: "ui.state", Field: true, Consts: []int64{32}, Doc: "the keyboard's focus is in the view and shows: it came from the keyboard, or it's in a field (tracked)"},
	{Recv: "ref", Name: "focus", Sig: "()", Builtin: "ui.focus", Doc: "gives the view the keyboard's focus: a field, a control, a focusable or pressable view; a window comes to the front"},
	{Recv: "ref", Name: "blur", Sig: "()", Builtin: "ui.blur"},
	{Recv: "ref", Name: "scrollTo", Sig: "(x: float, y: float, animated: bool = true)", Builtin: "ui.scroll_to", Doc: "scrolls a Scroll or a List to the content offset"},
	{Recv: "ref", Name: "scrollToEnd", Sig: "(animated: bool = true)", Builtin: "ui.scroll_to_end", Doc: "scrolls a Scroll or a List to its end (a horizontal Scroll's trailing edge)"},
	{Recv: "ref", Name: "scrollToItem", Sig: "(index: int, animated: bool = true)", Builtin: "ui.scroll_to_item", Doc: "scrolls a List's item index into view"},
	{Recv: "ref", Name: "zoomTo", Sig: "(scale: float, animated: bool = true)", Builtin: "ui.zoom_to", Doc: "zooms a zoomable Scroll's content to scale, within its minZoom and maxZoom (its zoomChange hears it)"},
	{Recv: "ref", Name: "play", Sig: "()", Builtin: "ui.play", Doc: "plays a Video"},
	{Recv: "ref", Name: "pause", Sig: "()", Builtin: "ui.pause"},
	{Recv: "ref", Name: "seek", Sig: "(seconds: float)", Builtin: "ui.seek"},
	{Recv: "ref", Name: "currentTime", Sig: "() -> float", Builtin: "ui.video_time", Field: true, Consts: []int64{0}, Doc: "a Video's time, in seconds from the start (tracked: it follows, every timeInterval while it plays)"},
	{Recv: "ref", Name: "duration", Sig: "() -> float", Builtin: "ui.video_time", Field: true, Consts: []int64{1}, Doc: "a Video's duration, in seconds: 0 until it's known, inf for a live stream (tracked)"},
	{Recv: "ref", Name: "status", Sig: "() -> VideoStatus", Builtin: "ui.video_status", Field: true, Doc: "a Video's playback (tracked)"},
	{Recv: "ref", Name: "enterPictureInPicture", Sig: "()", Builtin: "ui.video_action", Consts: []int64{0}, Doc: "a Video with pictureInPicture goes into picture in picture"},
	{Recv: "ref", Name: "exitPictureInPicture", Sig: "()", Builtin: "ui.video_action", Consts: []int64{1}},
	{Recv: "ref", Name: "enterFullScreen", Sig: "()", Builtin: "ui.video_action", Consts: []int64{2}, Doc: "a Video plays full screen, in the platform's player"},
	{Recv: "ref", Name: "exitFullScreen", Sig: "()", Builtin: "ui.video_action", Consts: []int64{3}},
	{Recv: "ref", Name: "eval", Sig: "(script: string) -> string", Builtin: "ui.web_eval_result", Async: true, Throws: true, Doc: "runs script in a WebView's page: its value, as JSON"},
	{Recv: "ref", Name: "run", Sig: "(script: string)", Builtin: "ui.web_eval", Doc: "runs script in a WebView's page, not waiting for it"},
	{Recv: "ref", Name: "goBack", Sig: "()", Builtin: "ui.web_navigate", Consts: []int64{0}},
	{Recv: "ref", Name: "goForward", Sig: "()", Builtin: "ui.web_navigate", Consts: []int64{1}},
	{Recv: "ref", Name: "reload", Sig: "()", Builtin: "ui.web_navigate", Consts: []int64{2}},
	{Recv: "ref", Name: "stopLoading", Sig: "()", Builtin: "ui.web_navigate", Consts: []int64{3}},
	{Recv: "ref", Name: "load", Sig: "(url: string)", Builtin: "ui.web_load", Doc: "loads url in a WebView now, the one it was given too once its page has moved on (a link followed): its url prop, unchanged, wouldn't"},
	{Recv: "ref", Name: "insert", Sig: "(text: string)", Builtin: "ui.insert_text", Doc: "types text into a TextField where its cursor is, over its selection"},
	{Recv: "ref", Name: "format", Sig: "(format: TextFormat, on: bool)", Builtin: "ui.format", Doc: "a rich TextField's selection (or what's typed next) in a style, or not"},
	{Recv: "ref", Name: "toggleFormat", Sig: "(format: TextFormat)", Builtin: "ui.toggle_format"},
	{Recv: "ref", Name: "setLink", Sig: "(url: string)", Builtin: "ui.set_link", Doc: "links a rich TextField's selection to url (\"\": unlinks it)"},

	// A Window's handle (Window(…) as w; any other view: the window it's
	// in). Its state is followed only once it's read.
	{Recv: "ref", Name: "show", Sig: "()", Builtin: "ui.window_do", Consts: []int64{0}, Doc: "shows the window (a closed Window opens)"},
	{Recv: "ref", Name: "hide", Sig: "()", Builtin: "ui.window_do", Consts: []int64{1}, Doc: "hides the window, keeping it and what it shows"},
	{Recv: "ref", Name: "minimize", Sig: "()", Builtin: "ui.window_do", Consts: []int64{3}},
	{Recv: "ref", Name: "maximize", Sig: "()", Builtin: "ui.window_do", Consts: []int64{4}},
	{Recv: "ref", Name: "restore", Sig: "()", Builtin: "ui.window_do", Consts: []int64{5}, Doc: "out of minimized or maximized"},
	{Recv: "ref", Name: "center", Sig: "()", Builtin: "ui.window_do", Consts: []int64{8}, Doc: "centers the window on its display"},
	{Recv: "ref", Name: "close", Sig: "()", Builtin: "ui.window_do", Consts: []int64{13}, Doc: "closes the window as the user does, asking first (window.close() doesn't)"},
	{Recv: "ref", Name: "setFullScreen", Sig: "(on: bool)", Builtin: "ui.window_toggle", Consts: []int64{6, 7}},
	{Recv: "ref", Name: "setAlwaysOnTop", Sig: "(on: bool)", Builtin: "ui.window_toggle", Consts: []int64{11, 12}},
	{Recv: "ref", Name: "setClickThrough", Sig: "(on: bool)", Builtin: "ui.window_toggle", Consts: []int64{14, 15}, Doc: "the mouse goes through the window to what's behind it, or doesn't"},
	{Recv: "ref", Name: "setSize", Sig: "(width: float, height: float)", Builtin: "ui.window_set", Consts: []int64{9}, Doc: "its content's size, in points"},
	{Recv: "ref", Name: "setPosition", Sig: "(x: float, y: float)", Builtin: "ui.window_set", Consts: []int64{10}, Doc: "its frame's top left on the screen, in points from the main display's"},
	{Recv: "ref", Name: "visible", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{1}, Doc: "the window shows, or is minimized (tracked)"},
	{Recv: "ref", Name: "focused", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{2}, Doc: "a view: the keyboard's focus is in it (it, or a view inside it); a window: it has the keyboard (tracked)"},
	{Recv: "ref", Name: "minimized", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{4}},
	{Recv: "ref", Name: "maximized", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{8}},
	{Recv: "ref", Name: "fullScreen", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{16}},
	{Recv: "ref", Name: "alwaysOnTop", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{32}},
	{Recv: "ref", Name: "clickThrough", Sig: "() -> bool", Builtin: "ui.window_flag", Field: true, Consts: []int64{64}},
	{Recv: "ref", Name: "position", Sig: "() -> (float, float)", Builtin: "ui.window_pair", Field: true, Consts: []int64{0}, Doc: "its frame's top left on the screen, in points (tracked)"},
	{Recv: "ref", Name: "size", Sig: "() -> (float, float)", Builtin: "ui.window_pair", Field: true, Consts: []int64{1}, Doc: "its content's size, in points (tracked)"},
	{Recv: "ref", Name: "windowState", Sig: "() -> WindowState", Builtin: "ui.window_state", Field: true, Doc: "all of its state at once (tracked)"},

	// The runtime's B-tree (Tree, BTree<K, V>: core/src/rt/tree.c), which
	// only the standard library names: std/collections' SortedMap's and
	// SortedSet's. by is the keys' order, a less-than (none: their own);
	// a set's tree (collections.newSet) has keys only. Extra "entryProto":
	// a frozen empty [(K, V)], whose elements' shape the runtime makes
	// entries by.
	{Recv: "tree", Name: "count", Sig: "() -> int", Builtin: "tree.count"},
	{Recv: "tree", Name: "get", Sig: "(key: K, by: (fn(K, K) -> bool)?) -> V?", Builtin: "tree.get", Fails: true},
	{Recv: "tree", Name: "has", Sig: "(key: K, by: (fn(K, K) -> bool)?) -> bool", Builtin: "tree.has", Fails: true},
	{Recv: "tree", Name: "put", Sig: "(key: K, value: V, by: (fn(K, K) -> bool)?) -> bool", Builtin: "tree.put", Inout: true, Fails: true, Doc: "gives key value: whether it's new"},
	{Recv: "tree", Name: "add", Sig: "(key: K, by: (fn(K, K) -> bool)?) -> bool", Builtin: "tree.add", Inout: true, Fails: true, Doc: "a set's: adds key, whether it's new"},
	{Recv: "tree", Name: "take", Sig: "(key: K, by: (fn(K, K) -> bool)?) -> V?", Builtin: "tree.take", Inout: true, Fails: true, Doc: "removes key: its value, none when it had none"},
	{Recv: "tree", Name: "delete", Sig: "(key: K, by: (fn(K, K) -> bool)?) -> bool", Builtin: "tree.delete", Inout: true, Fails: true, Doc: "removes key: whether it was there"},
	{Recv: "tree", Name: "near", Sig: "(key: K, by: (fn(K, K) -> bool)?, below: bool, equal: bool) -> (K, V)?", Builtin: "tree.near", Extra: "entryProto", Fails: true, Doc: "below: the entry with the greatest key less than key (or equal, with equal); else the least greater"},
	{Recv: "tree", Name: "nearKey", Sig: "(key: K, by: (fn(K, K) -> bool)?, below: bool, equal: bool) -> K?", Builtin: "tree.near_key", Fails: true},
	{Recv: "tree", Name: "end", Sig: "(last: bool) -> (K, V)?", Builtin: "tree.end", Extra: "entryProto", Doc: "the first entry (last: the last)"},
	{Recv: "tree", Name: "endKey", Sig: "(last: bool) -> K?", Builtin: "tree.end_key"},
	{Recv: "tree", Name: "entries", Sig: "(from: K?, after: bool, to: K?, by: (fn(K, K) -> bool)?, limit: int) -> [(K, V)]", Builtin: "tree.range", Extra: "proto", Consts: []int64{0}, Fails: true, Doc: "the entries from the first key not less than from (after: greater; none: the first) up to the first not less than to (none: the end), limit at most (negative: all)"},
	{Recv: "tree", Name: "keys", Sig: "(from: K?, after: bool, to: K?, by: (fn(K, K) -> bool)?, limit: int) -> [K]", Builtin: "tree.range", Extra: "proto", Consts: []int64{1}, Fails: true},
	{Recv: "tree", Name: "values", Sig: "(from: K?, after: bool, to: K?, by: (fn(K, K) -> bool)?, limit: int) -> [V]", Builtin: "tree.range", Extra: "proto", Consts: []int64{2}, Fails: true},
}

// The receivers' type variables.
var (
	tvT = &TypeParam{Name: "T", id: -10}
	tvK = &TypeParam{Name: "K", id: -11}
	tvV = &TypeParam{Name: "V", id: -12}
	tvU = &TypeParam{Name: "U", id: -13}
)

var methodIndex = func() map[string][]*Method {
	idx := map[string][]*Method{}
	Methods = append(Methods, seqMethods...)
	for _, m := range Methods {
		m.parse()
		idx[m.Recv+"."+m.Name] = append(idx[m.Recv+"."+m.Name], m)
	}
	return idx
}()

// methodSig is m's parameters and result, the prelude's types resolved
// in this checker's prelude.
func (c *Checker) methodSig(m *Method) ([]*Param, Type) {
	if !m.prelude {
		return m.Params, m.Result
	}
	if s, ok := c.sigs[m]; ok {
		return s.params, s.result
	}
	ps, res := c.parseSig(m.Sig, m.Recv+"."+m.Name)
	c.sigs[m] = parsedSig{ps, res}
	return ps, res
}

type parsedSig struct {
	params []*Param
	result Type
}

// parseSig reads a signature naming the prelude's types.
func (c *Checker) parseSig(sig, what string) ([]*Param, Type) {
	p := &sigParser{s: sig, lookup: func(name string) Type {
		if tn, ok := c.prelude.Scope.LookupLocal(name).(*TypeName); ok {
			c.resolveType(tn)
			return tn.Type
		}
		if tn, ok := c.universe.LookupLocal(name).(*TypeName); ok {
			return tn.Type
		}
		return nil
	}}
	ps := p.params()
	res := Type(Void)
	p.space()
	if strings.HasPrefix(p.s[p.i:], "->") {
		p.i += 2
		res = p.typ()
	}
	if p.err || p.unknown {
		panic("check: bad signature: " + what + " " + sig)
	}
	return ps, res
}

func recvKind(t Type) string {
	switch x := t.(type) {
	case *Array:
		return "array"
	case *Map:
		return "map"
	case *Set:
		return "set"
	case *Optional:
		return "optional"
	case *Task:
		return "task"
	case *Stream:
		return "stream"
	case *Seq:
		return "seq"
	case *Tree:
		return "tree"
	case *Named:
		if x.Obj == loadingT {
			return "loading"
		}
	case *Basic:
		switch x {
		case String:
			return "string"
		case Char:
			return "char"
		case Float:
			return "float"
		case Int:
			return "int"
		case I8, I16, I32, U8, U16, U32, U64:
			return "sized"
		case Duration:
			return "duration"
		case TestApp:
			return "app"
		case Bytes:
			return "bytes"
		case Color:
			return "color"
		case Graphics:
			return "graphics"
		case Path:
			return "path"
		case Ref:
			return "ref"
		}
	}
	return ""
}

// lookupMethod finds a built-in method of t named name (any arity).
func lookupMethod(t Type, name string) *Method {
	ms := methodIndex[recvKind(t)+"."+name]
	if len(ms) == 0 {
		return nil
	}
	return ms[0]
}

// lookupMethodArity finds the overload taking n arguments.
func lookupMethodArity(t Type, name string, n int) *Method {
	if ms := methodsArity(t, name, n); len(ms) > 0 {
		return ms[0]
	}
	return nil
}

// methodsArity lists the overloads taking n arguments (g.fill's by its
// paint's type).
func methodsArity(t Type, name string, n int) []*Method {
	var out []*Method
	for _, m := range methodIndex[recvKind(t)+"."+name] {
		if m.Field {
			continue
		}
		req := 0
		for _, p := range m.Params {
			if p.Default == nil {
				req++
			}
		}
		if n >= req && (n <= len(m.Params) || m.Variadic) {
			out = append(out, m)
		}
	}
	return out
}

// lookupField finds a built-in property of t (g.width, card.hovered).
func lookupField(t Type, name string) *Method {
	for _, m := range methodIndex[recvKind(t)+"."+name] {
		if m.Field {
			return m
		}
	}
	return nil
}

// methodNames lists t's built-in methods (for suggestions).
func methodNames(t Type) []string {
	var out []string
	prefix := recvKind(t) + "."
	for k := range methodIndex {
		if strings.HasPrefix(k, prefix) {
			out = append(out, strings.TrimPrefix(k, prefix))
		}
	}
	sort.Strings(out)
	return out
}

func (m *Method) params() []*Param { return m.Params }

// bind maps the receiver's type variables to its types.
func (m *Method) bind(recv Type) Subst {
	s := Subst{}
	switch x := recv.(type) {
	case *Array:
		s[tvT] = x.Elem
	case *Map:
		s[tvK], s[tvV] = x.Key, x.Elem
	case *Set:
		s[tvT] = x.Elem
	case *Optional:
		s[tvT] = x.Elem
	case *Task:
		s[tvT] = x.Elem
	case *Stream:
		s[tvT] = x.Elem
	case *Seq:
		seqBind(s, x)
	case *Tree:
		s[tvK], s[tvV] = x.Key, x.Elem
	case *Named:
		if len(x.Args) > 0 {
			s[tvT] = x.Args[0]
		}
	case *Basic:
		if IsSized(x) {
			s[tvT] = x
		}
	}
	return s
}

// requires reports what the receiver lacks for m, "" when nothing.
func (m *Method) requires(s Subst) string {
	t := s[tvT]
	switch m.Needs {
	case "Equatable":
		if !IsEquatable(t) {
			return "compares elements, and " + typeString(t) + " can't be compared: " + whyNotEquatable(t)
		}
	case "Comparable":
		if why := whyNotComparable(t); why != "" {
			return "() orders what's Comparable, and " + typeString(t) + " isn't: " + why + "; or order them with a less-than: " + m.Name + "((a, b) => …)"
		}
	case "Hashable":
		if why := whyNotHashable(t); why != "" {
			return "keeps each element once by its hash, and " + typeString(t) + " isn't Hashable: " + why
		}
	case "HashableU":
		if u := s[tvU]; u != nil && !IsInvalid(u) {
			if why := whyNotHashable(u); why != "" {
				return "(by:) makes a map's keys, and " + typeString(u) + " isn't Hashable: " + why
			}
		}
	case "number":
		if !IsNumeric(t) {
			return "adds numbers, and these are " + typeString(t) + "s"
		}
	case "signed":
		if !IsSigned(t) {
			return "takes a signed number: " + aType(t) + " has no negatives"
		}
	case "string":
		if t != String {
			return "joins strings, and these are " + typeString(t) + "s"
		}
	default:
		return m.seqRequires(s)
	}
	return ""
}

// parse reads m.Sig.
func (m *Method) parse() {
	p := &sigParser{s: m.Sig}
	m.Params = p.params()
	m.Result = Void
	p.space()
	if strings.HasPrefix(p.s[p.i:], "->") {
		p.i += 2
		m.Result = p.typ()
	}
	if p.err {
		panic("check: bad method signature: " + m.Recv + "." + m.Name + " " + m.Sig)
	}
	m.prelude = p.unknown
}

// sigParser reads the method table's signatures. lookup resolves a name
// that isn't built in (the prelude's types); without it such a name is
// Invalid, and unknown says one was seen.
type sigParser struct {
	s       string
	i       int
	err     bool
	unknown bool
	lookup  func(string) Type
}

func (p *sigParser) space() {
	for p.i < len(p.s) && p.s[p.i] == ' ' {
		p.i++
	}
}

func (p *sigParser) eat(tok string) bool {
	p.space()
	if strings.HasPrefix(p.s[p.i:], tok) {
		p.i += len(tok)
		return true
	}
	return false
}

func (p *sigParser) ident() string {
	p.space()
	j := p.i
	for p.i < len(p.s) && (p.s[p.i] >= 'a' && p.s[p.i] <= 'z' || p.s[p.i] >= 'A' && p.s[p.i] <= 'Z' || p.i > j && p.s[p.i] >= '0' && p.s[p.i] <= '9') {
		p.i++
	}
	if j == p.i {
		p.err = true
	}
	return p.s[j:p.i]
}

func (p *sigParser) params() []*Param {
	var out []*Param
	if !p.eat("(") {
		p.err = true
		return nil
	}
	for !p.eat(")") {
		if len(out) > 0 && !p.eat(",") {
			p.err = true
			return out
		}
		name := p.ident()
		if !p.eat(":") {
			p.err = true
			return out
		}
		pa := &Param{Name: name, Type: p.typ()}
		if p.eat("=") {
			p.space()
			j := strings.IndexAny(p.s[p.i:], ",)")
			if j < 0 {
				p.err = true
				return out
			}
			lit := strings.TrimSpace(p.s[p.i : p.i+j])
			p.i += j
			pa.Default = defaultLit(lit)
		}
		out = append(out, pa)
	}
	return out
}

func (p *sigParser) typ() Type {
	var t Type
	switch {
	case p.eat("["):
		k := p.typ()
		if p.eat(":") {
			v := p.typ()
			t = &Map{k, v}
		} else {
			t = &Array{k}
		}
		if !p.eat("]") {
			p.err = true
		}
	case p.eat("("):
		var es []Type
		for !p.eat(")") {
			if len(es) > 0 && !p.eat(",") {
				p.err = true
				return Invalid
			}
			es = append(es, p.typ())
		}
		if len(es) == 1 {
			t = es[0] // a type in parentheses: (fn(K, K) -> bool)?
		} else {
			t = &Tuple{es}
		}
	case p.eat("fn"):
		var ps []Type
		if !p.eat("(") {
			p.err = true
			return Invalid
		}
		for !p.eat(")") {
			if len(ps) > 0 && !p.eat(",") {
				p.err = true
				return Invalid
			}
			ps = append(ps, p.typ())
		}
		throws := p.eat("throws")
		r := Type(Void)
		if p.eat("->") {
			r = p.typ()
		}
		t = &FuncType{Params: ps, Result: r, Throws: throws}
	default:
		switch n := p.ident(); n {
		case "int":
			t = Int
		case "u8":
			t = U8
		case "float":
			t = Float
		case "bool":
			t = Bool
		case "string":
			t = String
		case "bytes":
			t = Bytes
		case "Color":
			t = Color
		case "Duration":
			t = Duration
		case "T":
			t = tvT
		case "K":
			t = tvK
		case "V":
			t = tvV
		case "U":
			t = tvU
		case "View":
			t = View
		case "Graphics":
			t = Graphics
		case "Path":
			t = Path
		case "Ref":
			t = Ref
		case "Stream", "Set", "Seq":
			if !p.eat("<") {
				p.err = true
				return Invalid
			}
			e := p.typ()
			switch n {
			case "Set":
				t = &Set{e}
			case "Seq":
				t = &Seq{e}
			default:
				t = &Stream{e}
			}
			if !p.eat(">") {
				p.err = true
			}
		case "TestApp":
			t = TestApp
		default:
			t = Invalid
			if n == "" {
				p.err = true
			} else if p.lookup == nil {
				p.unknown = true
			} else if t = p.lookup(n); t == nil {
				p.unknown = true
				t = Invalid
			}
		}
	}
	if p.eat("?") {
		t = MakeOptional(t)
	}
	return t
}

// defaultLit is a default written in a signature: "", a number, a bool
// or none.
func defaultLit(lit string) syntax.Expr {
	switch {
	case strings.HasPrefix(lit, `"`):
		return &syntax.StringLit{Segments: []string{strings.Trim(lit, `"`)}}
	case lit == "true" || lit == "false":
		return &syntax.BoolLit{Value: lit == "true"}
	case lit == "none":
		return &syntax.NoneLit{}
	case strings.Contains(lit, "."):
		return &syntax.BasicLit{Kind: syntax.FLOAT, Value: lit}
	}
	return &syntax.BasicLit{Kind: syntax.INT, Value: lit}
}
