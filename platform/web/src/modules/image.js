// The "image" module (platform/image): prefetch loads pictures into the
// host's memory cache before an Image shows them, with the headers given;
// clearCache forgets them (imagecache.js). Neither waits: each answers at
// once. The browser's HTTP cache (clearCache's 2, the disk's) can't be
// cleared from a page: what's kept there stays.
import { decodePrefetch, imageStore, absolute } from "../imagecache.js";
import { sourceURL } from "../media.js";

const none = new Uint8Array(0);

export function modules(h) {
  return {
    image: {
      prefetch(b) {
        const { urls, headers } = decodePrefetch(b);
        imageStore(h).prefetch(urls.map((u) => absolute(sourceURL(h, u))), headers);
        return none;
      },
      clearCache(b) {
        if (b[0] & 1) imageStore(h).clear();
        return none;
      },
    },
  };
}
