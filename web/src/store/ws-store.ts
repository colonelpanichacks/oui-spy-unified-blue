import { type Signal, signal } from "@preact/signals";

// ============================================================================
// WebSocket Store — single WS connection, signal-per-topic, auto-reconnect
// ============================================================================

const topics = new Map<string, Signal<unknown>>();
export const connected = signal(false);

let ws: WebSocket | null = null;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
let backoff = 1000;

const MAX_BACKOFF = 15000;

/** Get (or create) a signal for a WS topic */
export function topic<T>(name: string): Signal<T | null> {
  if (!topics.has(name)) topics.set(name, signal(null));
  return topics.get(name) as Signal<T | null>;
}

/** Send a command over the WS connection */
export function sendCommand(action: string, params?: Record<string, string>): void {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const msg: Record<string, unknown> = { c: action };
  if (params) msg.p = params;
  ws.send(JSON.stringify(msg));
}

/** Disconnect and stop reconnecting. */
export function disconnect(): void {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  if (ws) {
    ws.onclose = null; // Prevent reconnect
    ws.close();
    ws = null;
    connected.value = false;
  }
}

/** Connect to the WS endpoint. Call once at app startup. Returns disconnect fn. */
export function connect(): () => void {
  if (ws) return disconnect;

  const proto = "ws:";
  const host = location.host;
  // WS runs on HTTP server (port 80), not HTTPS
  const port80Host = host.includes(":") ? host.replace(/:\d+$/, ":80") : `${host}:80`;
  const url = `${proto}//${port80Host}/ws`;

  ws = new WebSocket(url);

  ws.onopen = () => {
    connected.value = true;
    backoff = 1000;
  };

  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data as string) as { t: string; d: unknown };
      if (msg.t && topics.has(msg.t)) {
        (topics.get(msg.t) as Signal<unknown>).value = msg.d;
      } else if (msg.t) {
        // Lazily create and set
        const s = topic(msg.t);
        s.value = msg.d;
      }
    } catch {
      // Ignore malformed messages
    }
  };

  ws.onclose = () => {
    connected.value = false;
    ws = null;
    scheduleReconnect();
  };

  ws.onerror = () => {
    // onclose will fire after onerror
  };

  return disconnect;
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    backoff = Math.min(backoff * 2, MAX_BACKOFF);
    connect();
  }, backoff);
}
