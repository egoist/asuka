package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/toml"
	"github.com/egoist/asuka/lang/check"
)

// Variants: asuka.toml's [variant.<name>] tables, each the app built
// another way (Flutter's flavors, EAS's build profiles, Gradle's product
// flavors): its keys are the file's, over its own, as asuka.toml is read
// (toml.UnmarshalOverlay), so a variant changes any key: its ID (or
// idSuffix), name, icon, defines, updates, platforms' tables. asuka dev,
// build, publish, run, test and check build one with --variant (or
// ASUKA_VARIANT). It isn't --profile, which is a provisioning profile's.

// variantKey is asuka.toml's key of the variants' tables.
const variantKey = "variant"

// variantEnv names the variant when --variant doesn't.
const variantEnv = "ASUKA_VARIANT"

// appVariant is --variant's ("": ASUKA_VARIANT's, else none);
// appDefines, --define's, by key.
var (
	appVariant string
	appDefines map[string]string
)

// variantName is what a variant may be called: it names a directory of
// dist and a table's key.
var variantName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]*$`)

// variantOnly are asuka.toml's keys a variant can't change: what's read
// of the file for every build, not the app's (its packages, its
// programs, its C, its catalogs).
var variantOnly = []string{"variant", "dependencies", "bin", "c", "i18n"}

// variantFlags adds --variant and --define to a command that builds the
// app; useVariant, after its flags are parsed, makes them the build's.
func variantFlags(fs *flagSet) {
	fs.String(&appVariant, "variant", 0, "", "build the app as asuka.toml's [variant.`name`]: its keys over the file's, as dev, staging or prod (default: "+variantEnv+", else none)")
	fs.Func("define", 0, "a build constant, `KEY=VALUE`, over asuka.toml's [defines], which @define(\"KEY\") reads (again for more)", func(s string) error {
		k, v, ok := strings.Cut(s, "=")
		if !ok || !check.DefineKey.MatchString(k) {
			return fmt.Errorf("--define %q: want KEY=VALUE, KEY letters, digits and _, not starting with a digit", s)
		}
		if appDefines == nil {
			appDefines = map[string]string{}
		}
		appDefines[k] = v
		return nil
	})
}

// useVariant makes --variant (or ASUKA_VARIANT) and --define the
// build's: what loadConfig and the compiler read of asuka.toml.
func useVariant() error {
	if appVariant == "" {
		appVariant = strings.TrimSpace(os.Getenv(variantEnv))
	}
	if appVariant != "" && !variantName.MatchString(appVariant) {
		return fmt.Errorf("--variant %q: a variant's name is letters, digits, - and _, starting with a letter or digit", appVariant)
	}
	appbuild.Variant, appbuild.Defines = appVariant, appDefines
	return nil
}

// readConfig decodes asuka.toml's data into c as the variant has it
// (name "": none), checking the variants' tables.
func readConfig(data []byte, c *appConfig, name string) error {
	var raw struct {
		Variant map[string]map[string]json.RawMessage `json:"variant"`
	}
	if err := toml.Unmarshal(data, &raw); err != nil {
		var te *toml.Error
		if errors.As(err, &te) {
			return fmt.Errorf("%s:%v", configFile, err)
		}
		return fmt.Errorf("%s: %v (want [variant.<name>] tables)", configFile, err)
	}
	for _, n := range slices.Sorted(maps.Keys(raw.Variant)) {
		if !variantName.MatchString(n) {
			return fmt.Errorf("%s: [variant.%s]: a variant's name is letters, digits, - and _, starting with a letter or digit", configFile, n)
		}
		for _, k := range variantOnly {
			if _, ok := raw.Variant[n][k]; ok {
				return fmt.Errorf("%s: [variant.%s]: %s is the project's, the same for every variant", configFile, n, k)
			}
		}
	}
	if err := toml.UnmarshalOverlay(data, c, variantKey, name); err != nil {
		var te *toml.Error
		if errors.As(err, &te) {
			return fmt.Errorf("%s:%v", configFile, err) // asuka.toml:3:9: what
		}
		return fmt.Errorf("%s: %v", configFile, err)
	}
	c.variant = name
	return nil
}

// variantOut is where a build of the variant goes in the app's dist: a
// directory of the variant's, so that each's builds are kept.
func variantOut(dist string) string {
	if appVariant == "" {
		return dist
	}
	return filepath.Join(dist, appVariant)
}
