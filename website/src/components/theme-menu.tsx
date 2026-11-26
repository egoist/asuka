import { MonitorIcon, MoonIcon, SunIcon } from "lucide-react"
import * as React from "react"

import { Button } from "@/components/ui/button"
import { DropdownMenu, DropdownMenuContent, DropdownMenuRadioGroup, DropdownMenuRadioItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu"
import { useMessages } from "@/lib/i18n"
import { readTheme, setTheme, themes, type Theme } from "@/lib/theme"

const icons = { system: MonitorIcon, light: SunIcon, dark: MoonIcon }

/**
 * The theme menu: System, the default, Light or Dark. Its button shows the
 * printing on screen, a sun or a moon, by <html>'s class (the page is
 * prerendered, so it can't know before it is in the browser).
 */
export function ThemeMenu() {
  const t = useMessages().theme
  // "system" until the page is in the browser; read again on opening, as another tab may have changed it.
  const [theme, setChoice] = React.useState<Theme>("system")
  React.useEffect(() => setChoice(readTheme()), [])

  return (
    <DropdownMenu modal={false} onOpenChange={(open) => open && setChoice(readTheme())}>
      <DropdownMenuTrigger render={<Button variant="ghost" size="icon" className="text-muted-foreground hover:text-foreground" aria-label={t.label} />}>
        <SunIcon className="dark:hidden" />
        <MoonIcon className="hidden dark:block" />
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" positionMethod="fixed" className="w-40">
        <DropdownMenuRadioGroup
          value={theme}
          onValueChange={(v) => {
            setChoice(v as Theme)
            setTheme(v as Theme)
          }}
        >
          {themes.map((value) => {
            const Icon = icons[value]
            return (
              <DropdownMenuRadioItem key={value} value={value} className="min-h-9">
                <Icon className="text-muted-foreground" />
                {t[value]}
              </DropdownMenuRadioItem>
            )
          })}
        </DropdownMenuRadioGroup>
      </DropdownMenuContent>
    </DropdownMenu>
  )
}
