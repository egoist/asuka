// The browser's time zone, for the core (core/src/abi/web.c): its IANA
// name, which std/time's Zone.local.name() says.

/** zoneName writes the zone's name into memory at p, cap bytes at most: its length, 0 when there's none or it doesn't fit. */
export function zoneName(memory, p, cap) {
  const b = new TextEncoder().encode(Intl.DateTimeFormat().resolvedOptions().timeZone || "");
  if (b.length > cap) return 0;
  new Uint8Array(memory.buffer, p, b.length).set(b);
  return b.length;
}
