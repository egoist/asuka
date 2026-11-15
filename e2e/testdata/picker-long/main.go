// Picker long shows a picker whose chosen item is wider than the picker.
package main

import (
	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

func App() ui.Node {
	chosen := signal.New(1)
	items := []string{"Solo", "Couple pack with two players and twelve rounds, 1280 yen"}
	return ui.SafeArea(ui.Style{Padding: ui.All(16), Gap: 12},
		ui.StaticText("ready"),
		ui.Column(ui.Style{Width: ui.Pt(220)},
			ui.Picker(ui.PickerProps{Items: ui.Val(items), Selected: chosen, Label: ui.Val("Pack")})),
	)
}

func main() { app.Run(App) }
