// Journal keeps a journal in iCloud (asuka/sdk/icloud): each day's entry
// is a text file in the app's Documents, which the user's other devices,
// the Files app and Finder show (in iCloud Drive's Journal folder); the
// text size is in iCloud's key-value store, the same on each device. An
// entry another device wrote is listed before it's downloaded, and
// downloaded as it opens; one two devices changed at once shows the
// conflict, and keeps the version the user picks.
//
// iCloud keeps an app's files under its own ID, so asuka dev builds the
// app rather than run it in the player. On the simulator, sign in to an
// Apple Account, with iCloud Drive on, in its Settings:
//
//	go run ./cmd/asuka dev --platform ios examples/journal
//
// On a Mac, iCloud needs the app signed with a provisioning profile
// that has iCloud (asuka build --profile; docs/icloud.md).
package main

import (
	"fmt"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/colors"
	"github.com/egoist/asuka/fonts"
	"github.com/egoist/asuka/sdk/icloud"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// today is the day's entry's name (a test's is fixed).
var today = func() string { return time.Now().Format("2006-01-02") }

// statusOf says where an entry is, if not simply on the device.
func statusOf(f icloud.File) string {
	switch {
	case f.Conflicts:
		return "Conflict"
	case f.Downloading:
		return fmt.Sprintf("Downloading %d%%", int(f.DownloadProgress*100))
	case f.Status == icloud.NotDownloaded:
		return "In iCloud"
	case f.Uploading:
		return "Uploading"
	}
	return ""
}

func Journal() ui.Node {
	account := icloud.UseAccount()
	files := icloud.UseFiles(icloud.DocumentsScope)
	size := icloud.Signal("textSize", 17.0)
	dir := signal.New("")  // the app's Documents in iCloud
	open := signal.New("") // the entry open, its path
	text := signal.New("")
	status := signal.New("")

	// run does iCloud's work off the main thread, and shows its error.
	run := func(f func() error) {
		go func() {
			if err := f(); err != nil {
				ui.Post(func() { status.Set(err.Error()) })
			}
		}()
	}
	// The Documents directory, once there's an account.
	signal.Effect(func() {
		if account.Get().Available && signal.Untrack(dir.Get) == "" {
			run(func() error {
				d, err := icloud.Documents()
				ui.Post(func() { dir.Set(d) })
				return err
			})
		}
	})
	// The entries, the newest first.
	entries := signal.NewMemoFunc(func() []icloud.File {
		var out []icloud.File
		for _, f := range files.Get() {
			if !f.Dir && strings.HasSuffix(f.Name, ".txt") {
				out = append(out, f)
			}
		}
		slices.SortFunc(out, func(a, b icloud.File) int { return strings.Compare(b.Name, a.Name) })
		return out
	}, nil)
	// current is the open entry's File, as iCloud lists it.
	current := signal.NewMemoFunc(func() icloud.File {
		for _, f := range entries.Get() {
			if f.Path == open.Get() {
				return f
			}
		}
		return icloud.File{}
	}, nil)

	read := func(path string) {
		open.Set(path)
		text.Set("")
		status.Set("Opening…")
		run(func() error {
			b, err := icloud.ReadFile(path) // downloaded first, if it isn't here
			ui.Post(func() {
				if err == nil && open.Peek() == path {
					text.Set(string(b))
					status.Set("")
				}
			})
			return err
		})
	}
	newEntry := func() {
		path := filepath.Join(dir.Peek(), today()+".txt")
		if slices.ContainsFunc(signal.Untrack(entries.Get), func(f icloud.File) bool { return f.Path == path }) {
			read(path)
			return
		}
		run(func() error {
			err := icloud.WriteFile(path, nil)
			ui.Post(func() {
				if err == nil {
					open.Set(path)
					text.Set("")
				}
			})
			return err
		})
	}
	save := func() {
		path, body := open.Peek(), text.Peek()
		run(func() error {
			err := icloud.WriteFile(path, []byte(body))
			if err == nil {
				ui.Post(func() { status.Set("Saved") })
			}
			return err
		})
	}
	// keep settles the open entry's conflict: this device's version, or
	// the other device's.
	keep := func(theirs bool) {
		path := open.Peek()
		run(func() error {
			var v *icloud.Version
			if theirs {
				vs, err := icloud.Conflicts(path)
				if err != nil || len(vs) == 0 {
					return err
				}
				v = &vs[0]
			}
			if err := icloud.Resolve(path, v); err != nil {
				return err
			}
			ui.Post(func() { read(path) })
			return nil
		})
	}

	hint := ui.Style{Font: fonts.Footnote, Color: colors.SecondaryLabel}
	return ui.SafeArea(ui.Style{Padding: ui.Symmetric(8, 16), Gap: 12},
		ui.StaticText("Journal", ui.Style{Font: fonts.LargeTitle.Weighted(fonts.Bold)}),
		ui.Show(signal.Func(func() bool { return !account.Get().Available }), func() ui.Node {
			return ui.StaticText("Sign in to iCloud, with iCloud Drive on, to keep your journal on all your devices.", hint)
		}),
		ui.Row(ui.Style{Gap: 8, Align: ui.AlignCenter},
			ui.Button(ui.ButtonProps{Title: ui.Val("New Entry"), Icon: ui.IconPlus, Variant: ui.ButtonFilled, OnPress: newEntry,
				Disabled: signal.Func(func() bool { return dir.Get() == "" })}),
			ui.Text(signal.Func(func() string { return fmt.Sprintf("Text size %g", size.Get()) }), ui.Style{Grow: 1, Align: ui.AlignEnd}),
			ui.Button(ui.ButtonProps{Title: ui.Val("A−"), OnPress: func() { size.Set(max(11, size.Peek()-1)) }}),
			ui.Button(ui.ButtonProps{Title: ui.Val("A+"), OnPress: func() { size.Set(min(34, size.Peek()+1)) }}),
		),
		ui.Scroll(ui.ScrollProps{Style: ui.Style{MaxHeight: ui.Pt(220)}},
			ui.For(entries, func(f icloud.File) string { return f.Path }, func(f signal.Accessor[icloud.File], _ signal.Accessor[int]) ui.Node {
				return ui.Row(ui.Style{Gap: 12, Padding: ui.Symmetric(6, 0), Align: ui.AlignCenter},
					ui.Button(ui.ButtonProps{Title: signal.Func(func() string { return strings.TrimSuffix(f.Get().Name, ".txt") }),
						OnPress: func() { read(signal.Untrack(f.Get).Path) }}),
					ui.Text(signal.Func(func() string { return statusOf(f.Get()) }), hint),
				)
			}),
		),
		ui.Show(signal.Func(func() bool { return open.Get() != "" }), func() ui.Node {
			return ui.Column(ui.Style{Gap: 8, Grow: 1},
				ui.Show(signal.Func(func() bool { return current.Get().Conflicts }), func() ui.Node {
					return ui.Row(ui.Style{Gap: 8, Align: ui.AlignCenter, Padding: ui.All(8), Background: colors.SecondaryBackground},
						ui.StaticText("Changed on another device too.", ui.Style{Grow: 1}),
						ui.Button(ui.ButtonProps{Title: ui.Val("Keep Mine"), OnPress: func() { keep(false) }}),
						ui.Button(ui.ButtonProps{Title: ui.Val("Keep Theirs"), OnPress: func() { keep(true) }}),
					)
				}),
				ui.TextField(ui.TextFieldProps{Value: text, Multiline: true, Placeholder: ui.Val("What happened today?"),
					Style: ui.StyleFunc(func() ui.Style {
						f := fonts.Body
						f.Size = float32(size.Get())
						return ui.Style{Grow: 1, Font: f}
					})}),
				ui.Row(ui.Style{Gap: 8, Align: ui.AlignCenter},
					ui.Text(status, ui.Style{Grow: 1, Font: fonts.Footnote, Color: colors.SecondaryLabel}),
					ui.Button(ui.ButtonProps{Title: ui.Val("Save"), Variant: ui.ButtonFilled, OnPress: save}),
				),
			)
		}),
	)
}

func main() { app.Run(Journal) }
