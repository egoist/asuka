package main

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"
)

// Interactive and configurable widgets: what a build adds for them.
//
// A widget's buttons and toggles (platform/widget's button, toggle,
// Layout.action) run an App Intent on iOS (platform/ios/Widget/Action.swift),
// compiled into the extension and into the app's App Intents, where the
// system runs it (launching the app in the background): the host hands
// the action to the app's code. On Android they're broadcasts to the
// host's AsukaWidgetActionReceiver, which starts the app's code without a
// screen.
//
// A kind's params (asuka.toml's widgets.params) make it configurable: on
// iOS 17 a WidgetConfigurationIntent of them (AppIntentConfiguration),
// which the system's Edit Widget shows; on Android the host's
// configuration screen (AsukaWidgetConfigureActivity), from the params as
// JSON.

// configWidgets are the iPhone's kinds that have params.
func (c *appConfig) configWidgets() []widgetConfig {
	var out []widgetConfig
	for _, w := range c.iosWidgets() {
		if w.configurable() {
			out = append(out, w)
		}
	}
	return out
}

// widgetParamDefault is a param's value when the user picked none, as
// platform/widget's configurations gives it: its default, else an options
// param's first option, a bool's false, "" for the others.
func widgetParamDefault(p shortcutParam) string {
	if p.Default != nil {
		return p.defaultText()
	}
	switch p.Type {
	case "options":
		return p.Options[0].Value
	case "bool":
		return "false"
	}
	return ""
}

// widgetConfigSwift is the configurable kinds' App Intents: each kind's
// configuration intent (AsukaWidgetConfig_<kind>) with a parameter for
// each param, an options param's AppEnum, and each entity's AppEntity and
// its query (platform/widget's setEntities, in the App Group). The
// extension and the app compile it (the app, to read its widgets'
// configurations); ext adds the extension's conformances.
func (c *appConfig) widgetConfigSwift(ext bool) string {
	ws := c.configWidgets()
	if len(ws) == 0 {
		return ""
	}
	const avail = "@available(iOS 17.0, macOS 14.0, watchOS 10.0, *)\n"
	var b strings.Builder
	entities := map[string]bool{}
	for _, w := range ws {
		for _, p := range w.Params {
			if p.Type != "entity" || entities[p.Entity] {
				continue
			}
			entities[p.Entity] = true
			fmt.Fprintf(&b, avail+`struct AsukaWidgetEntity_%[1]s: AppEntity {
    static let typeDisplayRepresentation = TypeDisplayRepresentation(name: %[2]s)
    static let defaultQuery = AsukaWidgetEntityQuery_%[1]s()
    let id: String
    let title: String
    let subtitle: String
    var displayRepresentation: DisplayRepresentation {
        subtitle.isEmpty ? DisplayRepresentation(title: "\(title)") : DisplayRepresentation(title: "\(title)", subtitle: "\(subtitle)")
    }
}

`+avail+`struct AsukaWidgetEntityQuery_%[1]s: EntityStringQuery {
    /// The app's things of the entity, as it last set them (setEntities).
    static func all() -> [AsukaWidgetEntity_%[1]s] {
        AsukaWidgetStore.entities(%[3]s).map { AsukaWidgetEntity_%[1]s(id: $0["id"] ?? "", title: $0["title"] ?? "", subtitle: $0["subtitle"] ?? "") }
    }
    func entities(for identifiers: [String]) async throws -> [AsukaWidgetEntity_%[1]s] {
        Self.all().filter { identifiers.contains($0.id) }
    }
    func entities(matching string: String) async throws -> [AsukaWidgetEntity_%[1]s] {
        Self.all().filter { $0.title.localizedCaseInsensitiveContains(string) }
    }
    func suggestedEntities() async throws -> [AsukaWidgetEntity_%[1]s] { Self.all() }
}

`, p.Entity, swiftString(p.Title), swiftString(p.Entity))
		}
	}
	for _, w := range ws {
		for _, p := range w.Params {
			if p.Type != "options" {
				continue
			}
			fmt.Fprintf(&b, avail+"enum AsukaWidgetParam_%s_%s: String, AppEnum {\n", w.Kind, p.Name)
			for i, o := range p.Options {
				fmt.Fprintf(&b, "    case o%d = %s\n", i, swiftString(o.Value))
			}
			fmt.Fprintf(&b, "    static let typeDisplayRepresentation = TypeDisplayRepresentation(name: %s)\n", swiftString(p.Title))
			b.WriteString("    static let caseDisplayRepresentations: [Self: DisplayRepresentation] = [\n")
			for i, o := range p.Options {
				fmt.Fprintf(&b, "        .o%d: DisplayRepresentation(title: %s),\n", i, swiftString(o.Title))
			}
			b.WriteString("    ]\n    var asukaTitle: String {\n        switch self {\n")
			for i, o := range p.Options {
				fmt.Fprintf(&b, "        case .o%d: return %s\n", i, swiftString(o.Title))
			}
			b.WriteString("        }\n    }\n}\n\n")
		}
		fmt.Fprintf(&b, avail+"struct AsukaWidgetConfig_%s: WidgetConfigurationIntent {\n    static let title: LocalizedStringResource = %s\n", w.Kind, swiftString(w.Name))
		if w.Description != "" {
			fmt.Fprintf(&b, "    static let description = IntentDescription(%s)\n", swiftString(w.Description))
		}
		var values, titles strings.Builder
		for _, p := range w.Params {
			typ := map[string]string{"string": "String", "int": "Int", "float": "Double", "bool": "Bool"}[p.Type]
			args := "title: " + swiftString(p.Title)
			optional := false
			def := p.Default
			switch {
			case p.Type == "options":
				typ = fmt.Sprintf("AsukaWidgetParam_%s_%s", w.Kind, p.Name)
				i := 0
				if def != nil {
					i = slices.IndexFunc(p.Options, func(o shortcutOption) bool { return o.Value == p.defaultText() })
				}
				args += fmt.Sprintf(", default: .o%d", i)
			case p.Type == "entity":
				typ = "AsukaWidgetEntity_" + p.Entity
				optional = true
			case p.Type == "bool" && def == nil:
				args += ", default: false"
			case def != nil:
				d := p.defaultText()
				if p.Type == "string" {
					d = swiftString(d)
				}
				args += ", default: " + d
			default:
				optional = true
			}
			if optional {
				typ += "?"
			}
			fmt.Fprintf(&b, "    @Parameter(%s)\n    var %s: %s\n", args, p.Name, typ)
			v, t := "", ""
			switch p.Type {
			case "options":
				v, t = p.Name+".rawValue", p.Name+".asukaTitle"
			case "entity":
				v, t = p.Name+"?.id ?? \"\"", p.Name+"?.title ?? \"\""
			case "bool":
				v, t = "String("+p.Name+")", `""`
			default:
				v = "String(describing: " + p.Name + ")"
				if optional {
					v = p.Name + ".map { String(describing: $0) } ?? \"\""
				}
				t = v
			}
			fmt.Fprintf(&values, "        v[%s] = %s\n", swiftString(p.Name), v)
			fmt.Fprintf(&titles, "        t[%s] = %s\n", swiftString(p.Name), t)
		}
		fmt.Fprintf(&b, "\n    init() {}\n\n    var asukaValues: [String: String] {\n        var v: [String: String] = [:]\n%s        return v\n    }\n\n    var asukaTitles: [String: String] {\n        var t: [String: String] = [:]\n%s        return t\n    }\n}\n\n", values.String(), titles.String())
		if ext {
			fmt.Fprintf(&b, avail+"extension AsukaWidgetConfig_%s: AsukaWidgetConfiguration {}\n\n", w.Kind)
		}
	}
	return b.String()
}

// widgetBridgeSwift is the app's side of the widgets (its App Intents
// module): what platform/widget's configurations asks the host for, its
// widgets' configurations (by name: the host is compiled before it).
func (c *appConfig) widgetBridgeSwift() string {
	var cases strings.Builder
	for _, w := range c.configWidgets() {
		fmt.Fprintf(&cases, "                case %s:\n                    if let c: AsukaWidgetConfig_%s = i.widgetConfigurationIntent() { out.append(c.asukaValues); continue }\n", swiftString(w.Kind), w.Kind)
	}
	return `/// The host's way to the app's widgets' configurations (by name, as the
/// intents are compiled after it): each widget of the kind's params, its
/// defaults where the user picked none.
@objc(AsukaWidgetBridge) final class AsukaWidgetBridge: NSObject {
    @objc static func configurations(_ kind: String, done: @escaping ([[String: String]]) -> Void) {
        WidgetCenter.shared.getCurrentConfigurations { result in
            var out: [[String: String]] = []
            for i in (try? result.get()) ?? [] where i.kind == kind {
                if #available(iOS 17.0, *) {
                    switch kind {
` + cases.String() + `                default: break
                    }
                }
                out.append(Self.defaults[kind] ?? [:])
            }
            DispatchQueue.main.async { done(out) }
        }
    }

    /// Each kind's params' defaults: a widget not configured yet has them.
    static let defaults: [String: [String: String]] = ` + c.widgetDefaultsSwift() + `
}

`
}

// widgetDefaultsSwift is each configurable kind's params' defaults, as a
// Swift dictionary literal.
func (c *appConfig) widgetDefaultsSwift() string {
	ws := c.configWidgets()
	if len(ws) == 0 {
		return "[:]"
	}
	var b strings.Builder
	b.WriteString("[")
	for i, w := range ws {
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString(swiftString(w.Kind) + ": [")
		for j, p := range w.Params {
			if j > 0 {
				b.WriteString(", ")
			}
			b.WriteString(swiftString(p.Name) + ": " + swiftString(widgetParamDefault(p)))
		}
		b.WriteString("]")
	}
	b.WriteString("]")
	return b.String()
}

// androidWidgetParams is a kind's params as the configuration screen
// reads them (JSON: name, title, type, default, options [{value, title}],
// entity), "" for a kind without.
func (w widgetConfig) androidParams() string {
	if !w.configurable() {
		return ""
	}
	type opt struct {
		Value string `json:"value"`
		Title string `json:"title"`
	}
	type param struct {
		Name     string `json:"name"`
		Title    string `json:"title"`
		Type     string `json:"type"`
		Default  string `json:"default"`
		Prompt   string `json:"prompt,omitempty"`
		Options  []opt  `json:"options,omitempty"`
		Entity   string `json:"entity,omitempty"`
		Optional bool   `json:"optional,omitempty"`
	}
	var ps []param
	for _, p := range w.Params {
		q := param{Name: p.Name, Title: p.Title, Type: p.Type, Default: widgetParamDefault(p), Prompt: p.Prompt, Entity: p.Entity,
			Optional: p.Default == nil && p.Type != "options" && p.Type != "bool"}
		for _, o := range p.Options {
			q.Options = append(q.Options, opt{o.Value, o.Title})
		}
		ps = append(ps, q)
	}
	b, _ := json.Marshal(ps)
	return string(b)
}
