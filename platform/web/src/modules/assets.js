// The "assets" module's "read" (fs.readAsset): one of the app's assets,
// as the site serves it.
const native = () => globalThis.asuka.native;

export function modules() {
  return {
    assets: {
      async read(b) {
        const name = new TextDecoder().decode(b);
        const r = await fetch(native().assetURL(name)).catch(() => null);
        if (!r?.ok) throw new Error(`no asset ${name}`);
        return new Uint8Array(await r.arrayBuffer());
      },
    },
  };
}
