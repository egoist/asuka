// The docs of the repository (../docs), read and rendered on the server. The
// site is prerendered, so this runs at build time only.

import fsSync from "node:fs"
import fs from "node:fs/promises"
import path from "node:path"
import type { List, Root } from "mdast"
import { toString } from "mdast-util-to-string"
import remarkParse from "remark-parse"
import { unified } from "unified"

import type { Doc, Navs, NavItem, NavSection, SearchEntry } from "@/lib/docs"
import { localeInfo, messages, type Locale } from "@/lib/i18n"
import { renderMarkdown, slugOf, type RenderedMarkdown } from "@/lib/markdown.server"
import { docsBase, isPublished } from "@/lib/markdown-source"
import { generatedGroups, referenceGroupOf, referencePackages, referenceSource, referenceSourceDir, writtenGroups } from "@/lib/reference"
import { site } from "@/lib/site"

// Vite runs in website/, next to the repository's docs.
const repoDir = path.resolve(process.cwd(), "..")

/** The docs of a language: docs/, docs/zh/, docs/ja/. */
function docsDirOf(locale: Locale) {
  return path.join(repoDir, localeInfo[locale].docsDir)
}

/** The pages of a language's docs, as files relative to them. */
async function pageFiles(locale: Locale) {
  return (await fs.readdir(docsDirOf(locale), { recursive: true })).filter((f) => isPublished(locale, f)).sort()
}

/** The files a page may be: x.md, else x/README.md. */
function filesOf(slug: string) {
  if (slug === "") return ["README.md"]
  if (!/^[a-z0-9-]+(\/[a-z0-9-]+)*$/.test(slug)) return []
  return [`${slug}.md`, `${slug}/README.md`]
}

/** The file of a page, relative to the docs, or undefined for none. */
async function fileOf(locale: Locale, slug: string) {
  for (const file of filesOf(slug)) {
    if (!isPublished(locale, file)) continue
    if (await fs.stat(path.join(docsDirOf(locale), file)).then((s) => s.isFile(), () => false)) return file
  }
  return undefined
}

// Pages are rendered once per build; in development, on every request.
const cache = new Map<string, Promise<RenderedMarkdown | undefined>>()

function render(locale: Locale, slug: string) {
  const key = `${locale}:${slug}`
  let page = cache.get(key)
  const group = referenceGroupOf(slug)
  if (!page && group && generatedGroups.includes(group)) {
    // A page of the reference: asuka doc's, or a list of packages.
    const docsDir = docsDirOf(locale)
    const source = referenceSource(repoDir, locale, slug)
    page = Promise.resolve(
      source === undefined
        ? undefined
        : renderMarkdown(source, path.join(docsDir, `${slug}.md`), { docsDir, repoDir, repoUrl: site.repo, base: docsBase(locale) }, { copyCode: messages[locale].docs.copyCode })
    )
    if (!import.meta.env.DEV) cache.set(key, page)
  }
  if (!page) {
    const docsDir = docsDirOf(locale)
    page = fileOf(locale, slug).then((file) => {
      if (!file) return undefined
      const abs = path.join(docsDir, file)
      return fs.readFile(abs, "utf8").then(
        (source) =>
          renderMarkdown(source, abs, { docsDir, repoDir, repoUrl: site.repo, base: docsBase(locale) }, { copyCode: messages[locale].docs.copyCode }),
        () => undefined
      )
    })
    if (!import.meta.env.DEV) cache.set(key, page)
  }
  return page
}

/**
 * The sidebar: the lists of the docs' README, in their order, under their
 * headings, then the pages it leaves out.
 */
export async function loadNav(locale: Locale): Promise<Navs> {
  const words = messages[locale].docs
  const readme = await fs.readFile(path.join(docsDirOf(locale), "README.md"), "utf8")
  const tree = unified().use(remarkParse).parse(readme) as Root
  const sections: NavSection[] = [{ title: words.overview, items: [{ slug: "", title: words.introduction }] }]
  let heading = ""
  for (const node of tree.children) {
    if (node.type === "heading" && node.depth === 2) heading = toString(node)
    if (node.type === "list" && heading) sections.push({ title: heading, items: navItems(node, ".") })
  }

  const listed = new Set(sections.flatMap((s) => s.items.map((i) => i.slug)))
  const rest: NavItem[] = []
  for (const file of await pageFiles(locale)) {
    const slug = slugOf(file)
    if (listed.has(slug) || referenceGroupOf(slug)) continue
    const page = await render(locale, slug)
    rest.push({ slug, title: page?.title || slug })
  }
  if (rest.length) sections.push({ title: words.more, items: rest })
  return [...sections, ...(await referenceNav(locale))]
}

/**
 * The reference's sidebar: the standard library's packages and the
 * platform SDK's, each set's list first. The docs' sidebar is the
 * sections of no part; a page of the reference shows its part's.
 */
async function referenceNav(locale: Locale): Promise<NavSection[]> {
  const words = messages[locale].reference
  const packages = referencePackages(repoDir)
  // The written parts (the language, the error codes): each README's
  // lists of pages, in order.
  const written = await Promise.all(
    writtenGroups.map(async (group): Promise<NavSection> => {
      const readme = await fs.readFile(path.join(docsDirOf(locale), group, "README.md"), "utf8")
      const tree = unified().use(remarkParse).parse(readme) as Root
      const pages = tree.children.flatMap((node) => (node.type === "list" ? navItems(node, group) : []))
      return {
        title: words[group as "language" | "errors"],
        reference: group,
        items: [{ slug: group, title: messages[locale].docs.overview, description: words[`${group as "language" | "errors"}About`] }, ...pages],
      }
    })
  )
  return [...written, ...generatedGroups.map((group) => ({
    title: words[group],
    reference: group,
    items: [
      { slug: group, title: messages[locale].docs.overview, description: words[`${group}About`] },
      ...packages.filter((p) => p.path.startsWith(group + "/")).map((p) => ({ slug: p.slug, title: p.path, description: p.description })),
    ],
  }) as NavSection)]
}

/** Where a page of the reference's source is: the package's directory, or for one the compiler provides, its documentation's file. */
function referenceSourceUrl(slug: string) {
  const dir = referenceSourceDir(slug)
  if (!dir) return `${site.repo}/tree/main/website/src/lib/reference.ts`
  if (fsSync.existsSync(path.join(repoDir, dir))) return `${site.repo}/tree/main/${dir}`
  return `${site.repo}/blob/main/lang/doc/builtin/${slug.split("/")[1]}.asuka`
}

function navItems(list: List, base: string): NavItem[] {
  return list.children.flatMap((item): NavItem[] => {
    const paragraph = item.children[0]
    if (paragraph?.type !== "paragraph") return []
    const [first, ...rest] = paragraph.children
    const description = sentence(rest.map((n) => toString(n)).join(""))
    if (first?.type === "link" && first.url.endsWith(".md") && !/^[a-z]+:/.test(first.url)) {
      return [{ slug: slugOf(path.join(base, first.url)), title: toString(first), description }]
    }
    return []
  })
}

/** "`: install the tools, then …`" → "Install the tools, then …". */
function sentence(text: string) {
  const s = text.replace(/^[\s:：]+/, "").replace(/\s+/g, " ").trim()
  return s.charAt(0).toUpperCase() + s.slice(1)
}

export async function loadDoc(locale: Locale, slug: string): Promise<Doc | undefined> {
  const reference = referenceGroupOf(slug)
  const [page, nav, file] = await Promise.all([render(locale, slug), loadNav(locale), reference ? undefined : fileOf(locale, slug)])
  if (!page || (!file && !reference)) return undefined
  // The page's sidebar: the docs', or its part of the reference's.
  const sections = nav.filter((s) => s.reference === reference)
  const order = sections.flatMap((s) => s.items).filter((i) => i.slug !== undefined)
  const index = order.findIndex((i) => i.slug === slug)
  const item = order[index]
  const link = (at: number) => {
    const i = order[at]
    return i?.slug === undefined ? undefined : { slug: i.slug, title: i.title, episode: at }
  }
  return {
    slug,
    title: page.title,
    description: item?.description || page.lead,
    summary: item?.description,
    section: sections.find((s) => s.items.includes(item!))?.title,
    episode: index >= 0 && !reference ? index : undefined,
    html: page.html,
    toc: page.toc,
    prev: index > 0 ? link(index - 1) : undefined,
    next: index >= 0 ? link(index + 1) : undefined,
    editUrl: file ? `${site.repo}/edit/main/${localeInfo[locale].docsDir}/${file.split(path.sep).join("/")}` : referenceSourceUrl(slug),
    reference,
    references: file ? await importedPackages(path.join(docsDirOf(locale), file)) : undefined,
  }
}

/**
 * The packages of the API reference a guide's examples import (import
 * std/http, import platform/purchase), in the order they first do: what
 * its page links to at the top.
 */
async function importedPackages(file: string): Promise<string[]> {
  const source = await fs.readFile(file, "utf8")
  const known = new Set(referencePackages(repoDir).map((p) => p.path))
  const out: string[] = []
  for (const m of source.matchAll(/^\s*import ((?:std|platform)\/[a-z0-9-]+)\b/gm)) {
    if (known.has(m[1]) && !out.includes(m[1])) out.push(m[1])
  }
  return out
}

/** Every heading of every page, with its text, for the search of the site. */
export async function loadSearchIndex(locale: Locale): Promise<SearchEntry[]> {
  const nav = await loadNav(locale)
  const entries: SearchEntry[] = []
  for (const item of nav.flatMap((s) => s.items)) {
    if (item.slug === undefined) continue
    const page = await render(locale, item.slug)
    if (!page) continue
    for (const section of page.sections) {
      entries.push({ slug: item.slug, page: page.title, id: section.id, heading: section.heading, text: section.text })
    }
  }
  return entries
}
