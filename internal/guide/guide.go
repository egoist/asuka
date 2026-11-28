// Package guide is what an Asuka project tells coding agents (rfcs/0015):
// the AGENTS.md asuka new writes, and asuka guide's page, the language
// on one page for the CLI's version: docs/language-tour.md and the
// language reference's pages joined, the workflow, and every diagnostic
// code's summary and fix. go generate ./cmd/asuka writes the page, in
// each of the docs' languages, into the CLI (cmd/asuka/guide), and a
// test there checks it's current.
package guide

import (
	"fmt"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/egoist/asuka/lang/diag"
)

// Site is where the docs are on the web.
const Site = "https://asuka.egoist.dev"

// A Kind is what asuka new makes.
type Kind int

const (
	App Kind = iota
	Program
	Library
)

// AgentsMD is the AGENTS.md asuka new writes into a new project of kind.
func AgentsMD(kind Kind) string {
	what, run := "an Asuka app", "; run: `asuka dev`"
	switch kind {
	case Program:
		what, run = "an Asuka program (a command-line tool)", "; run: `asuka run -- args`"
	case Library:
		what, run = "an Asuka library (a package apps import)", ""
	}
	return fmt.Sprintf(`# AGENTS.md

This is %s (%s). Before writing Asuka, read the
language for the installed version: `+"`asuka guide`"+`.

- Check: `+"`asuka check --json`"+` (one diagnostic per line, each with a code)
- Look up a code: `+"`asuka explain <code>`"+`
- Test: `+"`asuka test --json`"+`%s
- A component's body is declarations; state is written in handlers,
  fns, tasks and effects. Lists in views need `+"`key`"+`.
`, what, Site, run)
}

// words are the page's own, in each language.
var words = map[string]struct {
	title, intro, workflow, language, codes string
	steps                                   []string
}{
	"en": {
		title:    "Asuka, on one page",
		intro:    "This is the Asuka language as the installed CLI compiles it, and how to work with it. Every error and warning has a code; `asuka explain <code>` prints its page.",
		workflow: "Workflow", language: "The language", codes: "Diagnostic codes",
		steps: []string{
			"Check: `asuka check --json` (one diagnostic per line, each with a code, its message, its help and its page)",
			"Look up a code: `asuka explain <code>`",
			"Test: `asuka test --json` (each test's result, and what the core reports as the app runs); run: `asuka dev`, or `asuka run` for a program",
			"Format: `asuka fmt -w`",
			"A component's body is declarations; state is written in handlers, fns, tasks and effects. Lists in views need `key`.",
		},
	},
	"zh": {
		title:    "一页读懂 Asuka",
		intro:    "这是所装 CLI 编译的 Asuka 语言，以及使用它的方式。每个错误和警告都有一个代码；`asuka explain <code>` 会显示它的页面。",
		workflow: "工作流程", language: "语言", codes: "诊断代码",
		steps: []string{
			"检查：`asuka check --json`（每行一条诊断，各带代码、消息、帮助和页面）",
			"查询代码：`asuka explain <code>`",
			"测试：`asuka test --json`（每个测试的结果，以及应用运行时核心报告的问题）；运行：`asuka dev`，程序用 `asuka run`",
			"格式化：`asuka fmt -w`",
			"组件的主体是声明；状态在处理函数、fn、task 和 effect 里写入。视图里的列表需要 `key`。",
		},
	},
	"ja": {
		title:    "1 ページでわかる Asuka",
		intro:    "インストールした CLI がコンパイルする Asuka 言語と、その使い方です。すべてのエラーと警告にはコードがあり、`asuka explain <code>` でそのページを表示できます。",
		workflow: "作業の流れ", language: "言語", codes: "診断コード",
		steps: []string{
			"検査：`asuka check --json`（1 行に 1 つの診断。それぞれにコード、メッセージ、ヘルプ、ページ）",
			"コードを調べる：`asuka explain <code>`",
			"テスト：`asuka test --json`（各テストの結果と、アプリの実行中にコアが報告するもの）。実行：`asuka dev`、プログラムは `asuka run`",
			"整形：`asuka fmt -w`",
			"コンポーネントの本体は宣言です。状態はハンドラー、fn、タスク、エフェクトで書き込みます。ビューのリストには `key` が必要です。",
		},
	},
}

// Build is the guide in lang, from the docs of the repository at root.
func Build(root, lang string) (string, error) {
	w, ok := words[lang]
	if !ok {
		return "", fmt.Errorf("guide: no language %s", lang)
	}
	docs := "docs"
	if lang != "en" {
		docs = "docs/" + lang
	}
	read := func(rel string) (string, error) {
		b, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(path.Join(docs, rel))))
		return string(b), err
	}
	var b strings.Builder
	fmt.Fprintf(&b, "# %s\n\n%s\n\n## %s\n\n", w.title, w.intro, w.workflow)
	for _, s := range w.steps {
		fmt.Fprintf(&b, "- %s\n", s)
	}
	fmt.Fprintf(&b, "\n## %s\n", w.language)
	tour, err := read("language-tour.md")
	if err != nil {
		return "", err
	}
	b.WriteString("\n" + page(tour, "language-tour.md", lang))
	index, err := read("language/README.md")
	if err != nil {
		return "", err
	}
	for _, m := range listed.FindAllStringSubmatch(index, -1) {
		src, err := read("language/" + m[1])
		if err != nil {
			return "", err
		}
		b.WriteString("\n" + page(src, "language/"+m[1], lang))
	}
	fmt.Fprintf(&b, "\n## %s\n\n", w.codes)
	for _, c := range diag.All() {
		fmt.Fprintf(&b, "- %s\n", diag.Line(c, lang))
	}
	return b.String(), nil
}

// listed are the reference's pages, as its index lists them.
var listed = regexp.MustCompile(`(?m)^- \[[^\]]+\]\(([a-z-]+\.md)\)`)

var (
	heading = regexp.MustCompile(`(?m)^(#+) `)
	link    = regexp.MustCompile(`\]\(([^)#\s]*\.md)?(#[^)\s]*)?\)`)
	only    = regexp.MustCompile(`(?s)<!-- repository-only:start -->.*?<!-- repository-only:end -->\n?`)
)

// page is a page of the docs (file, relative to them) for the guide: its
// headings two levels down, under the guide's own; its links to the
// docs the site's; its checked examples plain Asuka.
func page(src, file, lang string) string {
	src = only.ReplaceAllString(src, "")
	src = strings.ReplaceAll(src, "```asuka check\n", "```asuka\n")
	var out []string
	fence := false
	for _, line := range strings.SplitAfter(src, "\n") {
		if strings.HasPrefix(line, "```") {
			fence = !fence
		}
		if !fence {
			line = heading.ReplaceAllString(line, "$1## ")
		}
		out = append(out, line)
	}
	src = strings.Join(out, "")
	base := Site + "/docs/"
	if lang != "en" {
		base = Site + "/" + lang + "/docs/"
	}
	return link.ReplaceAllStringFunc(src, func(m string) string {
		sm := link.FindStringSubmatch(m)
		target, anchor := sm[1], sm[2]
		if target == "" {
			return m // an anchor on the page
		}
		p := path.Clean(path.Join(path.Dir(file), target))
		if strings.HasPrefix(p, "..") {
			return m
		}
		p = strings.TrimSuffix(strings.TrimSuffix(p, ".md"), "/README")
		if p == "README" {
			p = ""
		}
		return "](" + base + p + anchor + ")"
	})
}
