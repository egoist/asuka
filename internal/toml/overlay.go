package toml

import (
	"fmt"
	"slices"
)

// An OverlayError says the table an overlay names isn't in the
// document: Have are the names its tables under Key do have, sorted.
type OverlayError struct {
	Key, Name string
	Have      []string
}

func (e *OverlayError) Error() string {
	if len(e.Have) == 0 {
		return fmt.Sprintf("no [%s.%s]: there's no [%s.<name>] table", e.Key, e.Name, e.Key)
	}
	return fmt.Sprintf("no [%s.%s]: there's %s", e.Key, e.Name, joinNames(e.Key, e.Have))
}

func joinNames(key string, names []string) string {
	s := ""
	for i, n := range names {
		switch {
		case i == 0:
		case i == len(names)-1:
			s += " and "
		default:
			s += ", "
		}
		s += "[" + key + "." + n + "]"
	}
	return s
}

// UnmarshalOverlay is Unmarshal of the document with one of the tables
// under its top-level key merged over its top level, and key itself left
// out: with key "variant" and name "staging", [variant.staging]'s keys
// are the document's, over its own. Tables merge key by key, all the way
// down; any other value (a string, an array, an array of tables)
// replaces the one it's over. name "" merges nothing: the document
// without key. A name the document has no table for is an
// *OverlayError; key or its table naming something not a table, an
// *Error where it is.
func UnmarshalOverlay(data []byte, v any, key, name string) error {
	root, err := parse(data)
	if err != nil {
		return err
	}
	over, ok := root.m[key]
	if ok {
		if over.kind != kTable {
			return &Error{over.line, over.col, fmt.Sprintf("%s: want a table, not %s", quoteKey(key), over.kind)}
		}
		root.keys = slices.DeleteFunc(slices.Clone(root.keys), func(k string) bool { return k == key })
		delete(root.m, key)
	}
	if name != "" {
		var o *value
		if ok {
			o = over.tbl.m[name]
		}
		if o == nil {
			e := &OverlayError{Key: key, Name: name}
			if ok {
				e.Have = slices.Sorted(slices.Values(over.tbl.keys))
			}
			return e
		}
		if o.kind != kTable {
			return &Error{o.line, o.col, fmt.Sprintf("%s.%s: want a table, not %s", quoteKey(key), quoteKey(name), o.kind)}
		}
		merge(root, o.tbl)
	}
	return decode(root, v)
}

// merge sets over's keys in t: a table in both merges into t's, and
// anything else replaces t's.
func merge(t, over *table) {
	for _, k := range over.keys {
		ov := over.m[k]
		tv, ok := t.m[k]
		if !ok {
			t.add(k, ov)
			continue
		}
		if tv.kind == kTable && ov.kind == kTable {
			// A copy: what's merged over is its own.
			nt := &table{keys: slices.Clone(tv.tbl.keys), m: make(map[string]*value, len(tv.tbl.m))}
			for kk, vv := range tv.tbl.m {
				nt.m[kk] = vv
			}
			merge(nt, ov.tbl)
			t.m[k] = &value{kind: kTable, line: tv.line, col: tv.col, tbl: nt}
			continue
		}
		t.m[k] = ov
	}
}
