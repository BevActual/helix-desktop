// The Tauri bridge, typed. Pages are built with `withGlobalTauri`, so `invoke` lives on `window.__TAURI__`.
declare global {
  interface Window {
    __TAURI__: {
      core: { invoke: <T>(command: string, args?: Record<string, unknown>) => Promise<T> };
      event: { listen: <T>(event: string, handler: (event: { payload: T }) => void) => Promise<() => void> };
    };
  }
}

export function invoke<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  return window.__TAURI__.core.invoke<T>(command, args);
}

export function listen<T>(event: string, handler: (payload: T) => void): Promise<() => void> {
  return window.__TAURI__.event.listen<T>(event, (event) => handler(event.payload));
}
