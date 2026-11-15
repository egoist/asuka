// Button Padding shows a button whose Padding replaces the platform's
// own (e2e's TestButtonPadding*): red, its title red too, under a
// magenta square 48 points high that the test measures it against.
package main

import (
	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/colors"
	"github.com/egoist/asuka/fonts"
	"github.com/egoist/asuka/ui"
)

func main() {
	red := colors.RGB(255, 0, 0)
	app.Run(func() ui.Node {
		return ui.View(ui.ViewProps{Style: ui.Style{
			Grow: 1, Justify: ui.JustifyCenter, Align: ui.AlignCenter, Gap: 24, Background: colors.RGB(255, 255, 255),
		}},
			ui.View(ui.ViewProps{Style: ui.Style{Width: ui.Pt(48), Height: ui.Pt(48), Background: colors.RGB(255, 0, 255)}}),
			ui.Button(ui.ButtonProps{Title: ui.Val("Continue"), Variant: ui.ButtonPlain, Style: ui.Style{
				MinHeight: ui.Pt(48), Padding: ui.Symmetric(10, 22),
				Background: red, Color: red, Font: fonts.Font{Size: 16, Weight: fonts.Bold},
			}}),
		)
	})
}
