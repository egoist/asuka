// platform/appicon on the web: the page's icon (its tab's), from the
// alternate icons the build hands the page (asuka.toml's alternateIcons,
// as data: URLs). The host keeps the choice and shows it again on the
// next visit (native.js's showIcon). An installed web app's icon is its
// manifest's: it stays.
export function makeIcons() {
  const app = () => globalThis.asuka.native.app;
  return {
    names() { return Object.keys(app().icons || {}).sort(); },
    current() { return app().savedIcon(); },
    async set(_signal, name) {
      if (!app().showIcon(name)) throw new Error("appicon: asuka.toml's alternateIcons has no icon of that name");
    },
  };
}
