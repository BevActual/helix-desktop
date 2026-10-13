import { useEffect, useRef } from "react";

/** Calls `load` once the element is (nearly) on screen and puts the picture it returns behind the element. */
export function useLazyBackground<T extends HTMLElement>(load: () => Promise<string | null | undefined>, margin = "250px") {
  const ref = useRef<T>(null);
  useEffect(() => {
    const element = ref.current;
    if (!element) return;
    let cancelled = false;
    const observer = new IntersectionObserver(
      (entries) => {
        if (!entries.some((entry) => entry.isIntersecting)) return;
        observer.disconnect();
        load().then((url) => {
          if (!cancelled && url) element.style.backgroundImage = `url(${url})`;
        });
      },
      { rootMargin: margin },
    );
    observer.observe(element);
    return () => {
      cancelled = true;
      observer.disconnect();
    };
    // `load` is a closure over the tile's identity; the tile is re-created when that changes (its React key).
  }, []);
  return ref;
}
