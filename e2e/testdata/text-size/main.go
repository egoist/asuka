// Text size is an e2e's app (e2e/textsize_test.go): two tabs, each a
// stack whose screen has text in the fonts apps use (a text style, a
// size, a weight), a pill and a button, while the system's text size
// changes.
package main

import (
	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/colors"
	"github.com/egoist/asuka/fonts"
	"github.com/egoist/asuka/ui"
)

func App() ui.Node {
	return ui.Tabs(ui.TabsProps{},
		ui.Tab{Title: ui.Val("One"), Icon: ui.IconHome, Content: func() ui.Node { return page("ready") }},
		ui.Tab{Title: ui.Val("Two"), Icon: ui.IconPerson, Content: func() ui.Node { return page("second") }},
	)
}

// page is a tab's stack: its screen's first text is first.
func page(first string) ui.Node {
	body := fonts.Font{Style: fonts.Body.Style, Size: 16}
	return ui.Stack(func() ui.Node {
		return ui.Scroll(ui.ScrollProps{Style: ui.Style{Grow: 1}},
			ui.Column(ui.Style{Padding: ui.All(16), Gap: 12},
				ui.StaticText(first),
				ui.StaticText("Title", ui.Style{Font: fonts.Title2.Weighted(fonts.Bold)}),
				ui.StaticText("A paragraph long enough to wrap onto a few lines, so a larger text size needs more of them.", ui.Style{Font: body}),
				ui.View(ui.ViewProps{Style: ui.Style{Padding: ui.Symmetric(3, 10), CornerRadius: 999, Background: colors.SecondaryBackground, AlignSelf: ui.AlignStart}},
					ui.StaticText("Pill", ui.Style{Font: fonts.Caption.Weighted(fonts.Semibold)}),
				),
				ui.Button(ui.ButtonProps{Title: ui.Val("Go"), Style: ui.Style{Font: body.Weighted(fonts.Bold)}}),
				ui.StaticText("Below", ui.Style{Font: fonts.Footnote}),
			),
		)
	}, ui.ScreenOptions{Title: ui.Val("Text size")})
}

func main() { app.Run(App) }
