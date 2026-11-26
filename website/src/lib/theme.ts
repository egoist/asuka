// The site's two printings, light and dark: the reader's choice, kept in
// their browser, or the system's (the default). <html> carries .light or
// .dark; the dark: variant and the palette (styles.css) follow the class.

export const themeKey = "asuka-theme"
export type Theme = "system" | "light" | "dark"
export const themes: Theme[] = ["system", "light", "dark"]

/**
 * Run in <head> before the page paints, so a reader never sees the other
 * printing first: the class from the choice, or from prefers-color-scheme,
 * which it keeps following while the choice is the system's.
 */
export const themeScript = `(function(){var d=document.documentElement,m=matchMedia("(prefers-color-scheme: dark)");function a(){var p;try{p=localStorage.getItem(${JSON.stringify(themeKey)})}catch(e){}var k=p==="dark"||(p!=="light"&&m.matches);d.classList.toggle("dark",k);d.classList.toggle("light",!k)}a();m.addEventListener("change",a);addEventListener("storage",function(e){if(e.key===${JSON.stringify(themeKey)})a()})})()`

export function readTheme(): Theme {
  try {
    const t = localStorage.getItem(themeKey)
    return t === "light" || t === "dark" ? t : "system"
  } catch {
    return "system"
  }
}

/** Keeps the choice and shows it: no transitions run while the colors swap. */
export function setTheme(theme: Theme) {
  try {
    if (theme === "system") localStorage.removeItem(themeKey)
    else localStorage.setItem(themeKey, theme)
  } catch {
    // Private windows may refuse: the choice then lasts as long as the page.
  }
  const dark = theme === "dark" || (theme === "system" && matchMedia("(prefers-color-scheme: dark)").matches)
  const root = document.documentElement
  root.classList.add("theme-switching")
  root.classList.toggle("dark", dark)
  root.classList.toggle("light", !dark)
  // After the next frame has been styled with the new colors.
  requestAnimationFrame(() => requestAnimationFrame(() => root.classList.remove("theme-switching")))
}
