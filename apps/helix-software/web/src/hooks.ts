import { useEffect, useState } from "react";
import { invoke } from "@helix/ui";

const cache = new Map<string, string | null>();
const waiting = new Map<string, Promise<string | null>>();

function fetchIcon(id: string): Promise<string | null> {
  const known = cache.get(id);
  if (known !== undefined) return Promise.resolve(known);
  let pending = waiting.get(id);
  if (!pending) {
    pending = invoke<string | null>("software_icon", { id })
      .catch(() => null)
      .then((uri) => {
        cache.set(id, uri);
        waiting.delete(id);
        return uri;
      });
    waiting.set(id, pending);
  }
  return pending;
}

/** An application's icon (a data URI from its remote's appstream icons), or null when it has none. Cached for the session. */
export function useIcon(id: string): string | null {
  const [uri, setUri] = useState<string | null>(cache.get(id) ?? null);
  useEffect(() => {
    let live = true;
    fetchIcon(id).then((value) => { if (live) setUri(value); });
    return () => { live = false; };
  }, [id]);
  return uri;
}

/** The icons are forgotten when the catalog is read again (an install may have brought new ones). */
export function forgetIcons(): void {
  cache.clear();
}
