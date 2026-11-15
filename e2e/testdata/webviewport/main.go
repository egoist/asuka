// Web viewport is an e2e's app (e2e/webviewport_test.go): a WebView
// whose page posts how tall 100vh, 100dvh and 100% are, and its
// innerHeight, on a line the test reads.
package main

import (
	"github.com/egoist/asuka/app"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// page measures the viewport's units with elements of those heights.
const page = `<!doctype html><meta name="viewport" content="width=device-width">
<style>body{margin:0} div{position:absolute;top:0;width:1px}</style>
<div id="vh" style="height:100vh"></div><div id="dvh" style="height:100dvh"></div>
<script>
function post() {
  const h = id => Math.round(document.getElementById(id).getBoundingClientRect().height);
  window.asuka.postMessage("vh " + h("vh") + " dvh " + h("dvh") + " inner " + innerHeight);
}
addEventListener("load", post);
addEventListener("resize", post);
</script>`

func App() ui.Node {
	got := signal.New("measured: none")
	return ui.Column(ui.Style{Grow: 1},
		ui.Text(got, ui.Style{Padding: ui.All(16)}),
		ui.WebView(ui.WebViewProps{HTML: ui.Val(page), OnMessage: func(m string) { got.Set("measured: " + m) }}),
	)
}

func main() { app.Run(App) }
