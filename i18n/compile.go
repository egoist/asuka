package i18n

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"slices"
	"strings"
)

// Compiled is a catalog compiled for the C core (core/src/ui/i18n.c),
// which an Asuka app's tr.* lookups say messages from: its messages
// parsed into small programs, with what the core needs of CLDR's data
// for the catalog's languages (their number symbols and patterns, their
// dates' patterns and names), so the core has no message parser, no
// pattern generator and no CLDR tables of its own.
type Compiled struct {
	// Blob is what the core loads (i18n.catalog), in the format below.
	Blob []byte
	// Keys are the messages' keys, sorted: a key's index is what tr.key
	// says it by (i18n.say).
	Keys []string
	// Args are each key's arguments, in the order the source language's
	// message has them: the tuple i18n.say takes, an int or a float for a
	// number, plural or selectordinal, an int for a date or a time
	// (nanoseconds since the Unix epoch), a string otherwise.
	Args [][]Arg
}

// CompileOptions say what to compile besides the messages.
type CompileOptions struct {
	// Dates adds every language's dates (their patterns and names) even
	// when no message has a date or a time: for formatDate.
	Dates bool
	// DateArgs names, by key, the arguments that are dates though the
	// message only writes them ({when}, which a DateTime placeholder is):
	// they're written as a short date and time.
	DateArgs map[string][]string
}

// The blob, little-endian, strings a u32 length and UTF-8:
//
//	"AKC4", string source language
//	u32 key count, per key a string
//	u32 skeleton count (of the messages' {d, date, ::skeleton}s)
//	u32 language count, per language:
//	  string tag, u8 flags (1: right to left), string autonym
//	  the number system: 10 strings (its digits), string decimal
//	  separator, string group separator, u8 minimum grouping digits;
//	  then the decimal pattern and the percent pattern, each string
//	  prefix, suffix, negative prefix, negative suffix, u8 primary
//	  group, u8 secondary group, u8 minimum integer digits, u8 maximum
//	  fraction digits
//	  the currencies: the currency pattern and the one of a symbol of
//	  letters next to the number, each a pattern as above (¤ in its
//	  affixes where the symbol goes), string the spacing between a digit
//	  and a symbol's letter, string amounts' decimal separator and
//	  string their group separator ("" where numbers' are); u32 count,
//	  per currency whose symbol isn't its code or that has its own
//	  pattern, sorted by code: string ISO 4217 code, string symbol, u8
//	  flags (1 its first rune is a letter, 2 its last: neither a symbol
//	  nor a space, what takes the spacing next to a digit; 4 its own
//	  pattern follows, as above), string its decimal separator, string
//	  its group separator ("" for the locale's)
//	  u8 dates (1: what follows; 0: the catalog writes no dates), then
//	    u8 its own hour cycle (12 or 24: its short time's)
//	    170 strings: the months (format then stand-alone; abbreviated,
//	    narrow, wide; 12 each), the weekdays (format then stand-alone;
//	    abbreviated, narrow, short, wide; 7 each, Sunday first), the
//	    day periods (abbreviated, narrow, wide; 12 each: am, pm,
//	    midnight, noon, morning1, ...), the eras (abbreviated, narrow,
//	    wide; BC then AD)
//	    5 strings: the zone's GMT format ("GMT{0}"), GMT's own name, the
//	    hour format ("+HH:mm;-HH:mm"), UTC's long and short names
//	    u8 day period rules (1: 24 u8s, each hour's period, then u8 1 if
//	    noon is said at noon; 0: none)
//	    50 strings: the patterns of the styles, by (date style * 5 + time
//	    style) * 2 + (0: 12 hours, 1: 24), styles 0 none, 1 short, 2
//	    medium, 3 long, 4 full
//	    the skeletons' patterns, 2 each (12 hours, 24)
//	    63 strings: the relative times, by (unit * 3 + width) * 3 +
//	    part, units second, minute, hour, day, week, month, year, widths
//	    long, short, narrow, parts the future patterns, the past ones
//	    (each 6, by Plural, "" for a category the language hasn't) and
//	    the named forms of -2 to 2 ("yesterday" is -1 day's; "" for
//	    none), each joined by U+001F
//	  u32 message count, per message u32 key index and its nodes
//
// Nodes are u32 count, then each u8 kind and:
//
//	0 text      string
//	1 argument  u8 index (in the key's Args): its value, a number
//	            written as a number is by default
//	2 pound     (in a plural's case: its number less the offset)
//	3 number    u8 index, u8 flags (1 percent, 2 integer, 4 no grouping,
//	            8 half even, 16 its fraction digits given, 32 an amount
//	            of a currency), u8 minimum fraction digits, u8 maximum;
//	            with 32, string the currency's ISO 4217 code
//	4 date      u8 index, u8 date style, u8 time style (as the styles'
//	            patterns are indexed; 255 255: a skeleton), u32 the
//	            skeleton's index when it is one
//	5 plural, 6 selectordinal
//	            u8 index, i64 offset, u32 case count, per case u8 type (0
//	            an exact value: then a string, its decimal; 1 a category:
//	            then u8 Plural) and its nodes
//	7 select    u8 index, u32 case count, per case a string and its nodes
//
// A message's argument the source's hasn't is written as its name in
// braces, as Format writes a missing argument. A date is written in the
// language shown's patterns (the user's hour cycle's), as Format writes
// one for the format locale.
const (
	cText = iota
	cArg
	cPound
	cNumber
	cDate
	cPlural
	cOrdinal
	cSelect
)

// Compile compiles the catalog's messages in each of its languages.
func (c *Catalog) Compile() (*Compiled, error) { return c.CompileWith(CompileOptions{}) }

// CompileWith compiles the catalog's messages as o says.
func (c *Catalog) CompileWith(o CompileOptions) (*Compiled, error) {
	langs := c.langs()
	if len(langs) == 0 {
		return nil, errors.New("i18n: Compile: a catalog of no language")
	}
	source := c.message
	keys := map[string]bool{}
	for _, lang := range langs {
		l := c.set.Load().data[lang]
		if l == nil {
			continue
		}
		l.once.Do(func() { l.msgs = l.load() })
		for k := range l.msgs {
			keys[k] = true
		}
	}
	out := &Compiled{}
	for k := range keys {
		out.Keys = append(out.Keys, k)
	}
	slices.Sort(out.Keys)
	// Each key's arguments: the source language's message's, then any
	// another language's has that it hasn't (written as {name}).
	for _, k := range out.Keys {
		var args []Arg
		if m := source(c.source, k); m != nil {
			args = m.Args()
		} else {
			for _, lang := range langs {
				if m := source(lang, k); m != nil {
					args = m.Args()
					break
				}
			}
		}
		out.Args = append(out.Args, args)
	}
	// The dates: the arguments each key writes as dates, the skeletons
	// its messages use.
	w := cWriter{dateArgs: map[string]bool{}}
	dates := o.Dates
	for i, k := range out.Keys {
		for _, a := range out.Args[i] {
			if a.Kind == "date" || a.Kind == "time" || slices.Contains(o.DateArgs[k], a.Name) {
				w.dateArgs[k+"\x00"+a.Name] = true
				dates = true
			}
		}
		for _, lang := range langs {
			if m := source(lang, k); m != nil {
				w.collectSkeletons(m.nodes)
			}
		}
	}
	w.b = append(w.b, "AKC4"...)
	w.str(c.source)
	w.u32(len(out.Keys))
	for _, k := range out.Keys {
		w.str(k)
	}
	w.u32(len(w.skeletons))
	w.u32(len(langs))
	for _, lang := range langs {
		w.str(lang)
		flags := byte(0)
		if Direction(lang) == RightToLeft {
			flags |= 1
		}
		w.b = append(w.b, flags)
		w.str(Autonym(lang))
		k := keyOf(lang)
		sys := k.dat().numbers(k.latn)
		for _, d := range sys.digits {
			w.str(d)
		}
		w.str(sys.decimalSep)
		w.str(sys.group)
		w.b = append(w.b, byte(sys.minGrouping))
		for _, p := range []*numberPattern{&sys.decimal, &sys.percent, &sys.currency, &sys.currencyAlpha} {
			w.pattern(p)
		}
		w.str(sys.currencySpacing)
		w.str(sys.currencyDecimal)
		w.str(sys.currencyGroup)
		curs := Currencies(lang)
		w.u32(len(curs))
		for _, c := range curs {
			w.str(c.Code)
			w.str(c.Symbol)
			w.b = append(w.b, c.flags())
			if c.Pattern != "" {
				p := parseNumberPattern(c.Pattern, sys)
				w.pattern(&p)
			}
			w.str(c.Decimal)
			w.str(c.Group)
		}
		if dates {
			w.b = append(w.b, 1)
			w.dates(lang)
		} else {
			w.b = append(w.b, 0)
		}
		var msgs []int
		for i, key := range out.Keys {
			if source(lang, key) != nil {
				msgs = append(msgs, i)
			}
		}
		w.u32(len(msgs))
		for _, i := range msgs {
			w.u32(i)
			w.key = out.Keys[i]
			if err := w.nodes(source(lang, out.Keys[i]).nodes, out.Args[i]); err != nil {
				return nil, fmt.Errorf("i18n: Compile: %s's %s: %w", lang, out.Keys[i], err)
			}
		}
	}
	out.Blob = w.b
	return out, nil
}

func (w *cWriter) pattern(p *numberPattern) {
	w.str(p.prefix)
	w.str(p.suffix)
	w.str(p.negPrefix)
	w.str(p.negSuffix)
	w.b = append(w.b, byte(p.primary), byte(p.secondary), byte(p.minInt), byte(p.maxFrac))
}

type cWriter struct {
	b         []byte
	key       string          // whose nodes are written
	dateArgs  map[string]bool // key NUL argument: a date
	skeletons []string
}

// collectSkeletons adds the skeletons of the date and time arguments of
// nodes to w's.
func (w *cWriter) collectSkeletons(nodes []node) {
	for i := range nodes {
		n := &nodes[i]
		if n.kind == nodeDate || n.kind == nodeTime {
			if skel, ok := strings.CutPrefix(n.style, "::"); ok {
				skel = strings.TrimSpace(skel)
				if !slices.Contains(w.skeletons, skel) {
					w.skeletons = append(w.skeletons, skel)
				}
			}
		}
		for j := range n.cases {
			w.collectSkeletons(n.cases[j].nodes)
		}
	}
}

// dates writes a language's dates: its names, its zone's formats, its
// day periods' rules, and the patterns of the styles and the skeletons,
// for each hour cycle.
func (w *cWriter) dates(lang string) {
	k := keyOf(lang)
	d := k.dates()
	// Its own hour cycle, its short time's: what a locale that names
	// none writes.
	switch patternHour(d.v[slotTimeFormats+ShortStyle.index()]) {
	case 'H', 'k':
		w.b = append(w.b, 24)
	default:
		w.b = append(w.b, 12)
	}
	for i := 0; i < slotDateFormats; i++ {
		w.str(d.v[i])
	}
	for i := 0; i < 5; i++ {
		w.str(d.v[slotZone+i])
	}
	if r := periodRules(k.lang, d.tag); r != nil {
		w.b = append(w.b, 1)
		for _, p := range r.byHour {
			w.b = append(w.b, byte(p))
		}
		noon := byte(0)
		if r.noon {
			noon = 1
		}
		w.b = append(w.b, noon)
	} else {
		w.b = append(w.b, 0)
	}
	cycles := [2]string{"-u-hc-h12", "-u-hc-h23"}
	base := stripExtensions(Normalize(lang))
	if IsPseudo(base) {
		base = "en"
	}
	for ds := Style(0); ds <= FullStyle; ds++ {
		for ts := Style(0); ts <= FullStyle; ts++ {
			for _, hc := range cycles {
				p := ""
				if ds != NoStyle || ts != NoStyle {
					p = NewDateFormatter(base+hc, DateOptions{Date: ds, Time: ts}).Pattern()
				}
				w.str(p)
			}
		}
	}
	for _, skel := range w.skeletons {
		for _, hc := range cycles {
			w.str(NewDateFormatter(base+hc, DateOptions{Skeleton: skel}).Pattern())
		}
	}
	for i := slotRelative; i < slotCount; i++ {
		w.str(d.v[i])
	}
}

func (w *cWriter) u32(n int) { w.b = binary.LittleEndian.AppendUint32(w.b, uint32(n)) }

func (w *cWriter) str(s string) {
	w.u32(len(s))
	w.b = append(w.b, s...)
}

func (w *cWriter) nodes(nodes []node, args []Arg) error {
	w.u32(len(nodes))
	for i := range nodes {
		if err := w.node(&nodes[i], args); err != nil {
			return err
		}
	}
	return nil
}

func (w *cWriter) node(n *node, args []Arg) error {
	if n.kind == nodeText {
		w.b = append(w.b, cText)
		w.str(n.text)
		return nil
	}
	if n.kind == nodePound {
		w.b = append(w.b, cPound)
		return nil
	}
	index := slices.IndexFunc(args, func(a Arg) bool { return a.Name == n.text })
	if index < 0 {
		// Not one of the source's arguments: written as Format writes a
		// missing one.
		w.b = append(w.b, cText)
		w.str("{" + n.text + "}")
		return nil
	}
	if index > math.MaxUint8 {
		return errors.New("more than 255 arguments")
	}
	switch n.kind {
	case nodeArg:
		if w.dateArgs[w.key+"\x00"+n.text] {
			// A date written as a value: its short date and time.
			w.b = append(w.b, cDate, byte(index), byte(ShortStyle), byte(ShortStyle))
			w.u32(0)
			break
		}
		w.b = append(w.b, cArg, byte(index))
	case nodeNumber:
		flags, minFrac, maxFrac, currency := numberStyle(n.style)
		w.b = append(w.b, cNumber, byte(index), flags, minFrac, maxFrac)
		if flags&32 != 0 {
			w.str(currency)
		}
	case nodeDate, nodeTime:
		w.b = append(w.b, cDate, byte(index))
		if skel, ok := strings.CutPrefix(n.style, "::"); ok {
			w.b = append(w.b, 255, 255)
			w.u32(slices.Index(w.skeletons, strings.TrimSpace(skel)))
			break
		}
		s := MediumStyle
		switch n.style {
		case "short":
			s = ShortStyle
		case "long":
			s = LongStyle
		case "full":
			s = FullStyle
		}
		if n.kind == nodeDate {
			w.b = append(w.b, byte(s), 0)
		} else {
			w.b = append(w.b, 0, byte(s))
		}
		w.u32(0)
	case nodePlural, nodeSelectOrdinal:
		kind := byte(cPlural)
		if n.kind == nodeSelectOrdinal {
			kind = cOrdinal
		}
		w.b = append(w.b, kind, byte(index))
		w.b = binary.LittleEndian.AppendUint64(w.b, uint64(n.offset))
		w.u32(len(n.cases))
		for i := range n.cases {
			c := &n.cases[i]
			if exact, ok := strings.CutPrefix(c.key, "="); ok {
				w.b = append(w.b, 0)
				w.str(exact)
			} else {
				p, ok := ParsePlural(c.key)
				if !ok {
					return fmt.Errorf("a case %q", c.key)
				}
				w.b = append(w.b, 1, byte(p))
			}
			if err := w.nodes(c.nodes, args); err != nil {
				return err
			}
		}
	case nodeSelect:
		w.b = append(w.b, cSelect, byte(index))
		w.u32(len(n.cases))
		for i := range n.cases {
			w.str(n.cases[i].key)
			if err := w.nodes(n.cases[i].nodes, args); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("a node of kind %d", n.kind)
	}
	return nil
}

// numberStyle is formatNumber's reading of a number style, as the core's
// flags: 1 percent, 2 integer, 4 no grouping, 8 half even, 16 its
// fraction digits given (then min and max), 32 an amount of currency.
func numberStyle(style string) (flags, minFrac, maxFrac byte, currency string) {
	stems := []string{style}
	if rest, ok := strings.CutPrefix(style, "::"); ok {
		stems = strings.Fields(rest)
	}
	for _, stem := range stems {
		switch {
		case stem == "integer" || stem == "precision-integer":
			flags |= 2 | 8
		case stem == "percent":
			flags |= 1 | 8
		case stem == "group-off":
			flags |= 4
		case strings.HasPrefix(stem, "currency/") && len(stem) == len("currency/")+3:
			flags |= 32
			currency = strings.ToUpper(stem[len("currency/"):])
		case strings.HasPrefix(stem, ".") && strings.Trim(stem[1:], "0#") == "" && len(stem) > 1:
			flags |= 16
			minFrac = byte(strings.Count(stem, "0"))
			maxFrac = byte(len(stem) - 1)
		}
	}
	return flags, minFrac, maxFrac, currency
}
