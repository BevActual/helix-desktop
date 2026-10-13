import { useEffect, useRef } from "react";

// `icon()` is the shared hand-drawn icon set (core/shared-ui/icons.js), loaded by the page before this bundle.
declare function icon(name: string): SVGElement;

export function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    host.current?.replaceChildren(icon(name));
  }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}
