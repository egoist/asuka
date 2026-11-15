// Field keyboard is an e2e's app (e2e/fieldkeyboard_test.go): a
// one-line field low in a scrolling page, where the keyboard covers it
// unless the page scrolls it into sight.
package main

import (
	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

func App() ui.Node {
	code := signal.New("")
	return ui.Scroll(ui.ScrollProps{Style: ui.Style{Grow: 1}},
		ui.SafeArea(ui.Style{Padding: ui.All(16), Gap: 14},
			ui.StaticText("ready"),
			ui.View(ui.ViewProps{Style: ui.Style{Height: ui.Pt(560)}}),
			ui.TextField(ui.TextFieldProps{Value: code, Placeholder: ui.Val("Traveler code")}),
			ui.Button(ui.ButtonProps{Title: ui.Val("Search"), Variant: ui.ButtonFilled}),
		),
	)
}

func main() { app.Run(App) }
