import { useEffect } from "react";
import { invoke } from "./tauri";

/** Reports where every named control is, so a nested test can click by name. Only runs when the host was started with a layout log. */
export function useLayoutReport(island: string): void {
  useEffect(() => {
    let timer: ReturnType<typeof setInterval> | undefined;
    invoke<boolean>("layout_reporting").then((wanted) => {
      if (!wanted) return;
      const send = () => {
        const elements: Record<string, { x: number; y: number; w: number; h: number }> = {};
        document.querySelectorAll<HTMLElement>("[data-test]").forEach((node) => {
          const box = node.getBoundingClientRect();
          if (box.width > 0 && box.height > 0 && getComputedStyle(node).visibility !== "hidden") {
            elements[node.dataset.test as string] = { x: box.x, y: box.y, w: box.width, h: box.height };
          }
        });
        invoke("report_layout", { report: { width: innerWidth, height: innerHeight, islands: [island], elements } }).catch(() => undefined);
      };
      send();
      timer = setInterval(send, 500);
    });
    return () => {
      if (timer) clearInterval(timer);
    };
  }, [island]);
}
