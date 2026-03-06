import type { SystemStatus } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { connected, topic } from "../../store/ws-store";

export function Header() {
  const wsStatus = topic<SystemStatus>("sys/status").value;
  const { data: pollStatus, connectionLost: httpLost } = usePoll<SystemStatus>("/api/status", 5000);

  const data = wsStatus ?? pollStatus;
  const isDisconnected = !connected.value && httpLost;

  const uptimeText = data
    ? (() => {
        const s = data.uptime;
        const m = Math.floor(s / 60);
        const h = Math.floor(m / 60);
        return h > 0 ? `UP ${h}h${String(m % 60).padStart(2, "0")}m` : `UP ${m}m`;
      })()
    : "";

  const heapText = data ? `HEAP ${Math.round(data.heap / 1024)}KB` : "";
  const sub = [uptimeText, heapText].filter(Boolean).join(" | ");

  return (
    <header class="flex-shrink-0 border-b-2 border-border-bright bg-bg-primary px-3 py-2">
      <h1 class="text-center text-xl font-bold tracking-[3px] text-accent">OUI SPY</h1>
      <div class="mt-0.5 text-center text-[10px] text-text-secondary">
        {isDisconnected ? (
          <span class="font-bold text-danger-bright">DISCONNECTED</span>
        ) : (
          sub || "UNIFIED FIRMWARE"
        )}
      </div>
    </header>
  );
}
