import { Link, useRouterState } from "@tanstack/react-router"
import { ChevronDownIcon, MenuIcon } from "lucide-react"

import { DocLink } from "@/components/docs/doc-link"
import { GitHubIcon } from "@/components/icons"
import { LanguageMenu } from "@/components/language-menu"
import { Mark, Wordmark } from "@/components/logo"
import { Search } from "@/components/search"
import { ThemeMenu } from "@/components/theme-menu"
import { Button, buttonVariants } from "@/components/ui/button"
import { DropdownMenu, DropdownMenuContent, DropdownMenuGroup, DropdownMenuItem, DropdownMenuLabel, DropdownMenuSeparator, DropdownMenuTrigger } from "@/components/ui/dropdown-menu"
import { messages, routes, useLocale } from "@/lib/i18n"
import { site } from "@/lib/site"

const navLink = "font-mono text-[12px] tracking-[0.14em] uppercase text-muted-foreground transition-colors hover:text-foreground data-[status=active]:text-unit-ink"
const mobileNavLink = "min-h-11 px-3 aria-[current=page]:bg-accent"

/** The reference's parts, each a page listing its pages. */
const referenceGroups = ["language", "errors", "std", "platform"] as const

export function SiteHeader() {
  const locale = useLocale()
  const t = messages[locale].nav
  const words = messages[locale].reference
  // On a page of the reference, References is where the reader is, not the docs.
  const inReference = useRouterState({ select: (s) => /\/docs\/(language|errors|std|platform)(\/|$)/.test(s.location.pathname) })
  const to = routes[locale]
  return (
    <header className="frame sticky top-0 z-40 flex h-14 items-center gap-6 border-b bg-background/80 px-4 backdrop-blur-xl sm:px-6">
      <Link to={to.home} className="flex items-center gap-2.5 text-[19px]">
        <Mark className="size-7" />
        <Wordmark />
      </Link>
      <nav className="hidden items-center gap-6 sm:flex">
        <Link to={to.docs} className={`${navLink} data-[reference]:text-muted-foreground!`} data-reference={inReference || undefined}>
          {t.docs}
        </Link>
        <DropdownMenu modal={false}>
          <DropdownMenuTrigger className={`${navLink} flex items-center gap-1 outline-none data-[current]:text-unit-ink`} data-current={inReference || undefined}>
            {t.references}
            <ChevronDownIcon className="size-3.5" />
          </DropdownMenuTrigger>
          <DropdownMenuContent align="start" positionMethod="fixed" className="w-72">
            {referenceGroups.map((group) => (
              <DropdownMenuItem key={group} className="flex-col items-start gap-0.5 py-2" render={<DocLink slug={group} />}>
                <span className="font-medium">{words[group]}</span>
                <span className="text-xs text-muted-foreground">{words[`${group}About`]}</span>
              </DropdownMenuItem>
            ))}
          </DropdownMenuContent>
        </DropdownMenu>
        <a href={`${site.repo}/tree/main/examples`} className={navLink}>
          {t.examples}
        </a>
      </nav>
      <div className="ml-auto flex items-center gap-1">
        <Search />
        <LanguageMenu />
        <ThemeMenu />
        <a href={site.repo} aria-label="GitHub" className={buttonVariants({ variant: "ghost", size: "icon" })}>
          <GitHubIcon />
        </a>
        <DropdownMenu modal={false}>
          <DropdownMenuTrigger render={<Button variant="ghost" size="icon" className="sm:hidden" aria-label={t.menu} />}>
            <MenuIcon />
          </DropdownMenuTrigger>
          <DropdownMenuContent align="end" positionMethod="fixed" className="w-52 sm:hidden">
            <DropdownMenuItem className={mobileNavLink} render={<Link to={to.docs} />}>
              {t.docs}
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuGroup>
              <DropdownMenuLabel>{t.references}</DropdownMenuLabel>
              {referenceGroups.map((group) => (
                <DropdownMenuItem key={group} className={mobileNavLink} render={<DocLink slug={group} />}>
                  {words[group]}
                </DropdownMenuItem>
              ))}
            </DropdownMenuGroup>
            <DropdownMenuSeparator />
            <DropdownMenuItem className={mobileNavLink} render={<a href={`${site.repo}/tree/main/examples`} />}>
              {t.examples}
            </DropdownMenuItem>
          </DropdownMenuContent>
        </DropdownMenu>
      </div>
    </header>
  )
}
