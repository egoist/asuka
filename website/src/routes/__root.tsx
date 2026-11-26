import { HeadContent, Outlet, Scripts, createRootRoute } from "@tanstack/react-router"
import * as React from "react"

import { NotFound } from "@/components/not-found"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { localeInfo, localeScript, useLocale } from "@/lib/i18n"
import { site } from "@/lib/site"
import { themeScript } from "@/lib/theme"
import appCss from "@/styles.css?url"

export const Route = createRootRoute({
  head: () => ({
    meta: [
      { charSet: "utf-8" },
      { name: "viewport", content: "width=device-width, initial-scale=1" },
      { title: site.title },
      { name: "description", content: site.description },
      { property: "og:type", content: "website" },
      { property: "og:site_name", content: site.name },
      { property: "og:title", content: site.title },
      { property: "og:description", content: site.description },
      { name: "twitter:card", content: "summary" },
    ],
    links: [
      { rel: "stylesheet", href: appCss },
      { rel: "icon", href: "/favicon.svg", type: "image/svg+xml" },
    ],
    // Before anything shows: the page in the reader's language, and in
    // their theme (the class on <html>, which hydration leaves as it is).
    scripts: [
      { children: themeScript },
      { children: localeScript },
      // Analytics, in production only.
      ...(import.meta.env.PROD
        ? [{ defer: true, "data-domain": "asuka.egoist.dev", src: "https://tt.egoist.dev/js/script.js" }]
        : []),
    ],
  }),
  shellComponent: RootDocument,
  component: RootLayout,
  notFoundComponent: NotFound,
})

function RootDocument({ children }: { children: React.ReactNode }) {
  const locale = useLocale()
  return (
    <html lang={localeInfo[locale].lang} suppressHydrationWarning>
      <head>
        <HeadContent />
      </head>
      <body>
        {children}
        <Scripts />
      </body>
    </html>
  )
}

function RootLayout() {
  return (
    <div className="flex min-h-svh flex-col">
      <SiteHeader />
      <div className="frame flex flex-1 flex-col">
        <Outlet />
      </div>
      <SiteFooter />
    </div>
  )
}
