// Control labels names editable controls without grouping them in a View.
package main

import (
	"fmt"

	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

func App() ui.Node {
	text := signal.New("Ada")
	on := signal.New(false)
	checked := signal.New(false)
	label := signal.New("Accept terms")
	fieldLabel, notesLabel, richLabel := signal.New("Name"), signal.New("Notes"), signal.New("Formatted notes")
	return ui.SafeArea(ui.Style{Padding: ui.All(16), Gap: 12},
		ui.StaticText("Name"),
		ui.TextField(ui.TextFieldProps{Value: text, Label: fieldLabel, Placeholder: ui.Val("Enter a name")}),
		ui.StaticText("Notes"),
		ui.TextField(ui.TextFieldProps{Value: signal.New("A note"), Label: notesLabel, Multiline: true}),
		ui.StaticText("Formatted notes"),
		ui.TextField(ui.TextFieldProps{Rich: signal.NewFunc(ui.Rich{ui.Strong("Rich note")}, nil), Label: richLabel}),
		ui.Row(ui.Style{Gap: 12},
			ui.Toggle(ui.ToggleProps{Value: on, Label: ui.Val("Notifications")}),
			ui.StaticText("Send notifications")),
		ui.Row(ui.Style{Gap: 12},
			ui.Toggle(ui.ToggleProps{Value: checked, Label: label, Checkbox: true}),
			ui.Text(signal.Func(func() string { return "Checkbox: " + label.Get() }))),
		ui.Text(signal.Func(func() string { return fmt.Sprintf("values: %s %t %t", text.Get(), on.Get(), checked.Get()) })),
		ui.Button(ui.ButtonProps{Title: ui.Val("Set values"), OnPress: func() {
			text.Set("Grace")
			on.Set(true)
			checked.Set(true)
		}}),
		ui.Button(ui.ButtonProps{Title: ui.Val("Rename"), OnPress: func() {
			label.Set("Agree to terms")
			fieldLabel.Set("Full name")
			notesLabel.Set("Comments")
			richLabel.Set("Formatted comments")
		}}),
		ui.Button(ui.ButtonProps{Title: ui.Val("Clear labels"), OnPress: func() {
			label.Set("")
			fieldLabel.Set("")
			notesLabel.Set("")
			richLabel.Set("")
		}}),
		ui.StaticText("ready"),
	)
}

func main() { app.Run(App) }
