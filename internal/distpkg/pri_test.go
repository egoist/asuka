package distpkg

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// makepriProject is the PRI makepri indexes a project folder as (makepri
// new with createconfig's configuration, without its resource packs):
// the files of Assets, by name, their qualifiers in their names
// (Square44x44Logo.targetsize-16_altform-unplated.png), then the strings
// of Strings\<language>\Resources.resw, by language, then the files at
// the root.
func makepriProject(name, def string, assets []string, strs map[string][][2]string, root []string) *PRI {
	p := &PRI{Name: name, DefaultLanguage: def}
	sort.Slice(assets, func(i, j int) bool { return strings.ToUpper(assets[i]) < strings.ToUpper(assets[j]) })
	byName := map[string]int{}
	for _, f := range assets {
		ext := filepath.Ext(f)
		base := strings.TrimSuffix(f, ext)
		res := base
		var qs []PRIQualifier
		if i := strings.IndexByte(base, '.'); i >= 0 {
			res = base[:i]
			for _, q := range strings.Split(base[i+1:], "_") {
				k, v, _ := strings.Cut(q, "-")
				t := map[string]PRIQualifierType{"scale": PRIScale, "targetsize": PRITargetSize, "altform": PRIAlternateForm, "contrast": PRIContrast, "theme": PRITheme, "lang": PRILanguage}[k]
				qs = append(qs, PRIQualifier{t, v})
			}
		}
		key := "Files/Assets/" + res + ext
		i, ok := byName[key]
		if !ok {
			i = len(p.Resources)
			byName[key] = i
			p.Resources = append(p.Resources, PRIResource{Name: key})
		}
		p.Resources[i].Candidates = append(p.Resources[i].Candidates, PRICandidate{Qualifiers: qs, Value: `Assets\` + f, Path: true})
	}
	var langs []string
	for l := range strs {
		langs = append(langs, l)
	}
	sort.Slice(langs, func(i, j int) bool { return strings.ToUpper(langs[i]) < strings.ToUpper(langs[j]) })
	for _, l := range langs {
		for _, kv := range strs[l] {
			key := "Resources/" + kv[0]
			i, ok := byName[key]
			if !ok {
				i = len(p.Resources)
				byName[key] = i
				p.Resources = append(p.Resources, PRIResource{Name: key})
			}
			p.Resources[i].Candidates = append(p.Resources[i].Candidates, PRICandidate{Qualifiers: []PRIQualifier{{PRILanguage, l}}, Value: kv[1]})
		}
	}
	for _, f := range root {
		p.Resources = append(p.Resources, PRIResource{Name: "Files/" + f, Candidates: []PRICandidate{{Value: f, Path: true}}})
	}
	return p
}

// The projects testdata/makepri-*.pri were made of, by makepri.exe
// 10.0.28000 (the Windows SDK's build tools): its output again, byte for
// byte.
var makepriProjects = map[string]*PRI{
	"ref1": makepriProject("dev.asuka.pritest", "en-US",
		[]string{"Square44x44Logo.targetsize-16.png", "Square44x44Logo.targetsize-24.png", "Square44x44Logo.targetsize-16_altform-unplated.png", "Square44x44Logo.targetsize-24_altform-unplated.png", "Square44x44Logo.scale-100.png", "Square44x44Logo.scale-200.png", "Square150x150Logo.scale-100.png", "Square150x150Logo.scale-200.png", "StoreLogo.scale-100.png", "StoreLogo.scale-200.png"},
		map[string][][2]string{
			"en-US":   {{"AppName", "Counter"}, {"Description", "Counts things"}},
			"de":      {{"AppName", "Zähler"}, {"Description", "Zählt Dinge"}},
			"zh-Hant": {{"AppName", "計數器"}, {"Description", "計數"}},
			"ar":      {{"AppName", "العداد"}, {"Description", "يعد الأشياء"}},
		}, []string{"AppxManifest.xml", "priconfig.xml"}),
	"ref2": func() *PRI {
		var assets []string
		for _, s := range []string{"100", "125", "150", "200", "400"} {
			for _, n := range []string{"Square44x44Logo", "Square150x150Logo", "Wide310x150Logo", "StoreLogo"} {
				assets = append(assets, n+".scale-"+s+".png")
			}
		}
		for _, t := range []string{"16", "24", "32", "48", "256"} {
			assets = append(assets, "Square44x44Logo.targetsize-"+t+".png", "Square44x44Logo.targetsize-"+t+"_altform-unplated.png", "Square44x44Logo.targetsize-"+t+"_altform-lightunplated.png")
		}
		return makepriProject("dev.asuka.pritest", "en", assets, map[string][][2]string{
			"en": {{"AppName", "Counter"}}, "en-GB": {{"AppName", "Counter GB"}}, "pt": {{"AppName", "Contador"}}, "pt-BR": {{"AppName", "Contador BR"}},
			"zh-Hans": {{"AppName", "计数器"}}, "zh-Hant": {{"AppName", "計數器"}}, "sr-Latn": {{"AppName", "Brojač"}}, "ar": {{"AppName", "العداد"}},
		}, []string{"AppxManifest.xml", "priconfig.xml"})
	}(),
	"ref3": makepriProject("dev.asuka.pritest", "en-US",
		[]string{"X.contrast-high.png", "X.contrast-standard.png", "Y.contrast-black.png", "Y.contrast-white.png", "Z.theme-light.png", "Z.theme-dark.png", "Z.theme-light_contrast-high.png", "W.scale-300.png", "W.scale-100_contrast-high.png", "V.targetsize-20.png", "V.targetsize-256_altform-unplated.png", "V.altform-lightunplated_targetsize-32.png"},
		nil, []string{"AppxManifest.xml", "priconfig.xml"}),
	"ref4": makepriProject("dev.asuka.pritest", "en-US",
		[]string{"Logo.scale-100.png", "Logo.scale-200.png", "StoreLogo.scale-100.png", "StoreLogo.scale-200.png"},
		map[string][][2]string{"en-US": {{"AppName", "Counter"}, {"Name", "Counter"}, {"Other", "ter"}, {"Ünï", "x"}}},
		[]string{"AppxManifest.xml", "priconfig.xml"}),
	// Equal values: a resource's in several languages, and two
	// resources' in two languages, each stored once, in the section of
	// the first language that has it.
	"ref5": makepriProject("dev.asuka.pritest", "en", nil, map[string][][2]string{
		"en": {{"A", "Y"}, {"B", "same"}, {"C", "c2"}}, "ar": {{"A", "X"}, {"B", "same"}, {"C", "d"}},
		"de": {{"A", "X"}, {"B", "d"}, {"C", "c"}}, "zh-Hant": {{"A", "z"}, {"B", "same"}, {"C", "c3"}},
	}, nil),
}

func TestPRIMakepri(t *testing.T) {
	for name, p := range makepriProjects {
		t.Run(name, func(t *testing.T) {
			want, err := os.ReadFile(filepath.Join("testdata", "makepri-"+name+".pri"))
			if err != nil {
				t.Fatal(err)
			}
			got, err := WritePRI(p)
			if err != nil {
				t.Fatal(err)
			}
			wp, err := readPRI(want)
			if err != nil {
				t.Fatalf("makepri's: %v", err)
			}
			gp, err := readPRI(got)
			if err != nil {
				t.Fatalf("ours: %v", err)
			}
			if w, g := wp.raw()+wp.dump(), gp.raw()+gp.dump(); w != g {
				t.Errorf("ours:\n%s\nmakepri's:\n%s", diffLines(g, w), "")
			}
			if !bytes.Equal(got, want) {
				for i := range min(len(got), len(want)) {
					if got[i] != want[i] {
						t.Errorf("%d bytes, makepri's %d; the first difference at %#x: %x, makepri's %x", len(got), len(want), i, got[i:min(i+16, len(got))], want[i:min(i+16, len(want))])
						break
					}
				}
				if len(got) != len(want) {
					t.Errorf("%d bytes, makepri's %d", len(got), len(want))
				}
			}
		})
	}
}

// diffLines shows the lines of a that b doesn't have, and b's that a
// doesn't.
func diffLines(a, b string) string {
	al, bl := strings.Split(a, "\n"), strings.Split(b, "\n")
	in := func(l []string) map[string]int {
		m := map[string]int{}
		for _, s := range l {
			m[s]++
		}
		return m
	}
	am, bm := in(al), in(bl)
	var out strings.Builder
	for _, s := range al {
		if bm[s] == 0 {
			fmt.Fprintf(&out, "+ %s\n", s)
		} else {
			bm[s]--
		}
	}
	for _, s := range bl {
		if am[s] == 0 {
			fmt.Fprintf(&out, "- %s\n", s)
		} else {
			am[s]--
		}
	}
	return out.String()
}

// A PRI read back: every resource by its name, every candidate's
// qualifiers and value as given, each value in the type that's shortest
// (ASCII, UTF-8, UTF-16), the candidates in MRT's order, equal values
// stored once, scopes nested, names out of ASCII.
func TestPRIRoundTrip(t *testing.T) {
	lang := func(l string) []PRIQualifier { return []PRIQualifier{{PRILanguage, l}} }
	p := &PRI{Name: "Example.Notes", DefaultLanguage: "en-US", Resources: []PRIResource{
		{Name: "Files/Assets/Logo.png", Candidates: []PRICandidate{
			{Qualifiers: []PRIQualifier{{PRIScale, "100"}}, Value: `Assets\Logo.scale-100.png`, Path: true},
			{Qualifiers: []PRIQualifier{{PRIScale, "200"}, {PRIContrast, "high"}}, Value: `Assets\Logo.scale-200_contrast-high.png`, Path: true},
			{Qualifiers: []PRIQualifier{{PRITheme, "light"}}, Value: `Assets\Logo.theme-light.png`, Path: true},
			{Qualifiers: []PRIQualifier{{PRITargetSize, "24"}, {PRIAlternateForm, "unplated"}}, Value: `Assets\Logo.targetsize-24_altform-unplated.png`, Path: true},
			{Qualifiers: []PRIQualifier{{PRITargetSize, "24"}}, Value: `Assets\Logo.targetsize-24.png`, Path: true},
		}},
		{Name: "Files/Assets/Ünïcode.png", Candidates: []PRICandidate{{Value: `Assets\Ünïcode.png`, Path: true}}},
		{Name: "Resources/AppName", Candidates: []PRICandidate{
			{Qualifiers: lang("en-US"), Value: "Notes"}, {Qualifiers: lang("de"), Value: "Notizen"},
			{Qualifiers: lang("ja"), Value: "メモ"}, {Qualifiers: lang("ar"), Value: "ملاحظات"}, {Qualifiers: lang("en-GB"), Value: "Notes"},
		}},
		{Name: "Resources/Errors/NotFound", Candidates: []PRICandidate{
			{Qualifiers: lang("en-US"), Value: "Not found"}, {Qualifiers: lang("de"), Value: "Notizen"},
		}},
		{Name: "Resources/Empty", Candidates: []PRICandidate{{Qualifiers: lang("en-US"), Value: ""}}},
	}}
	b, err := WritePRI(p)
	if err != nil {
		t.Fatal(err)
	}
	r, err := readPRI(b)
	if err != nil {
		t.Fatal(err)
	}
	if r.schema.name != "Example.Notes" || r.schema.uniqueName != "ms-appx://Example.Notes/" {
		t.Errorf("the map %q %q", r.schema.name, r.schema.uniqueName)
	}
	// Each resource's candidates as read: their qualifiers, type and value.
	got := map[string][]string{}
	for i, n := range r.schema.items {
		for _, c := range r.items[i].candidates {
			var qs []string
			for _, q := range r.sets[c.set] {
				x := r.qualifiers[q]
				qs = append(qs, priTypeNames[x.typ]+"="+x.value)
			}
			v, err := c.text()
			if err != nil {
				t.Errorf("%s: %v", r.fullName(n), err)
			}
			got[r.fullName(n)] = append(got[r.fullName(n)], fmt.Sprintf("[%s] %s %s", strings.Join(qs, " "), priValueTypeNames[c.valueType], v))
		}
	}
	want := map[string][]string{
		"Files/Assets/Logo.png": {
			"[Contrast=HIGH Scale=200] AsciiPath Assets\\Logo.scale-200_contrast-high.png",
			"[Theme=LIGHT] AsciiPath Assets\\Logo.theme-light.png",
			"[TargetSize=24 AlternateForm=UNPLATED] AsciiPath Assets\\Logo.targetsize-24_altform-unplated.png",
			"[TargetSize=24] AsciiPath Assets\\Logo.targetsize-24.png",
			"[Scale=100] AsciiPath Assets\\Logo.scale-100.png",
		},
		"Files/Assets/Ünïcode.png": {"[] Utf8Path Assets\\Ünïcode.png"},
		"Resources/AppName": {
			"[Language=ja] String メモ", "[Language=en-US] AsciiString Notes", "[Language=en-GB] AsciiString Notes",
			"[Language=de] AsciiString Notizen", "[Language=ar] Utf8String ملاحظات",
		},
		"Resources/Errors/NotFound": {"[Language=en-US] AsciiString Not found", "[Language=de] AsciiString Notizen"},
		"Resources/Empty":           {"[Language=en-US] AsciiString "},
	}
	for name, w := range want {
		if strings.Join(got[name], "\n") != strings.Join(w, "\n") {
			t.Errorf("%s:\n%s\nwant\n%s", name, strings.Join(got[name], "\n"), strings.Join(w, "\n"))
		}
	}
	if len(got) != len(want) {
		t.Errorf("%d resources: %v", len(got), got)
	}
	// Equal values are one data item: en-US's and en-GB's Notes, and
	// AppName's and NotFound's Notizen.
	same := func(a, b string, i, j int) bool {
		ca, cb := r.itemNamed(a).candidates[i], r.itemNamed(b).candidates[j]
		return ca.section == cb.section && ca.index == cb.index
	}
	if !same("Resources/AppName", "Resources/AppName", 1, 2) || !same("Resources/AppName", "Resources/Errors/NotFound", 3, 1) {
		t.Error("equal values stored twice")
	}
	// The default context's scores: the default language's 1, a region
	// of it 0.98, another language 0; 100% scale 1, 200% 0.75.
	for _, q := range r.qualifiers[1:] {
		want := map[string]int{"Language=en-US": 1000, "Language=en-GB": 980, "Language=de": 0, "Scale=100": 1000, "Scale=200": 750,
			"Contrast=HIGH": 0, "Theme=LIGHT": 0, "TargetSize=24": 500, "AlternateForm=UNPLATED": 0, "Language=ja": 0, "Language=ar": 0}[priTypeNames[q.typ]+"="+q.value]
		if q.fallbackScore != want {
			t.Errorf("%s=%s scores %d, want %d", priTypeNames[q.typ], q.value, q.fallbackScore, want)
		}
	}
	// Writing it again gives the same bytes.
	if b2, _ := WritePRI(p); !bytes.Equal(b, b2) {
		t.Error("two writes differ")
	}
}

func (p *priRead) itemNamed(name string) priReadItem {
	for i, n := range p.schema.items {
		if p.fullName(n) == name {
			return p.items[i]
		}
	}
	return priReadItem{}
}

// Language tags scored against the default as makepri scores them.
func TestPRILanguageScore(t *testing.T) {
	for _, c := range []struct {
		def, v string
		want   uint16
	}{
		{"en-US", "en-US", 1000}, {"en", "EN", 1000}, {"he", "iw", 1000},
		{"en", "en-GB", 980}, {"en-US", "en", 980}, {"en-US", "en-GB", 980}, {"en-GB", "en-AU", 980}, {"en-GB", "en-001", 980}, {"zh-Hant", "zh-TW", 980}, {"zh-Hans", "zh-CN", 980},
		{"es-419", "es-MX", 990},
		{"pt-BR", "pt-PT", 970}, {"es-419", "es-ES", 970}, {"zh-TW", "zh-HK", 970},
		{"zh-Hans", "zh-Hant", 0}, {"sr-Latn", "sr-Cyrl", 0}, {"en", "de", 0}, {"nb", "nn", 0},
	} {
		if got := priLanguageScore(c.def, c.v); got != c.want {
			t.Errorf("%s for %s: %d, want %d (makepri's)", c.v, c.def, got, c.want)
		}
	}
}

func TestPRIErrors(t *testing.T) {
	str := func(v string) []PRICandidate {
		return []PRICandidate{{Qualifiers: []PRIQualifier{{PRILanguage, "en"}}, Value: v}}
	}
	for name, p := range map[string]*PRI{
		"no name":             {Resources: []PRIResource{{Name: "Resources/A", Candidates: str("a")}}},
		"twice":               {Name: "n", Resources: []PRIResource{{Name: "Resources/A", Candidates: str("a")}, {Name: "resources/a", Candidates: str("b")}}},
		"a scope and an item": {Name: "n", Resources: []PRIResource{{Name: "Resources/A", Candidates: str("a")}, {Name: "Resources/A/B", Candidates: str("b")}}},
		"an empty part":       {Name: "n", Resources: []PRIResource{{Name: "Resources//A", Candidates: str("a")}}},
		"no candidates":       {Name: "n", Resources: []PRIResource{{Name: "Resources/A"}}},
		"a scale Windows hasn't": {Name: "n", Resources: []PRIResource{{Name: "Files/A.png", Candidates: []PRICandidate{
			{Qualifiers: []PRIQualifier{{PRIScale, "175"}}, Value: "A.scale-175.png", Path: true}}}}},
		"a qualifier twice": {Name: "n", Resources: []PRIResource{{Name: "Files/A.png", Candidates: []PRICandidate{
			{Qualifiers: []PRIQualifier{{PRIScale, "100"}, {PRIScale, "200"}}, Value: "A.png", Path: true}}}}},
		"two candidates for the same": {Name: "n", Resources: []PRIResource{{Name: "Resources/A", Candidates: append(str("a"), str("b")...)}}},
		"a bad language":              {Name: "n", Resources: []PRIResource{{Name: "Resources/A", Candidates: []PRICandidate{{Qualifiers: []PRIQualifier{{PRILanguage, "not a tag!"}}, Value: "a"}}}}},
	} {
		if _, err := WritePRI(p); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
}

// ASUKA_PRI_DUMP=file.pri prints a PRI's tables and resources, makepri's
// as well as ours.
func TestPRIDump(t *testing.T) {
	f := os.Getenv("ASUKA_PRI_DUMP")
	if f == "" {
		t.Skip("ASUKA_PRI_DUMP names a PRI to print")
	}
	b, err := os.ReadFile(f)
	if err != nil {
		t.Fatal(err)
	}
	p, err := readPRI(b)
	if err != nil {
		t.Fatal(err)
	}
	t.Log("\n" + p.raw() + "\n" + p.dump())
}
