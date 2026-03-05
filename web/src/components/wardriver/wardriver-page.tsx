import { useEffect, useState } from "preact/hooks";
import type {
  WardriverDevice,
  WardriverFilter,
  WardriverSighting,
  WardriverStatus,
} from "../../api/client";
import { postEmpty, postForm } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { Button } from "../shared/button";
import { Card } from "../shared/card";
import { DeviceCard, Tag } from "../shared/device-card";
import { EmptyState } from "../shared/empty-state";
import { StatCard } from "../shared/stat-card";
import { TextArea } from "../shared/text-input";
import { useToast } from "../shared/toast";

interface DevicesResponse {
  devices: WardriverDevice[];
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

export function WardriverPage() {
  const { toast } = useToast();
  const { data: status } = usePoll<WardriverStatus>("/api/wardriver/status", 2000);
  const { data: recent } = usePoll<WardriverSighting[]>("/api/wardriver/recent", 3000);
  const { data: filters } = usePoll<WardriverFilter[]>("/api/wardriver/filters", 10000);
  const { data: devicesResp } = usePoll<DevicesResponse>("/api/wardriver/devices", 3000);

  const [ouis, setOuis] = useState("");
  const [macs, setMacs] = useState("");
  const [filtersLoaded, setFiltersLoaded] = useState(false);

  useEffect(() => {
    if (filters && !filtersLoaded) {
      const ouiList: string[] = [];
      const macList: string[] = [];
      for (const f of filters) {
        if (f.full) macList.push(f.id);
        else ouiList.push(f.id);
      }
      setOuis(ouiList.join("\n"));
      setMacs(macList.join("\n"));
      setFiltersLoaded(true);
    }
  }, [filters, filtersLoaded]);

  const startSession = async () => {
    try {
      await postEmpty("/api/wardriver/start");
      toast("Session started", "success");
    } catch {
      toast("Failed to start session", "error");
    }
  };

  const stopSession = async () => {
    try {
      await postEmpty("/api/wardriver/stop");
      toast("Session stopped", "success");
    } catch {
      toast("Failed to stop session", "error");
    }
  };

  const clearData = async () => {
    if (!confirm("Clear all wardriver data and CSV?")) return;
    try {
      await postEmpty("/api/wardriver/clear");
      toast("Data cleared", "success");
    } catch {
      toast("Failed to clear data", "error");
    }
  };

  const saveFilters = async () => {
    try {
      const result = await postForm<{ saved: number }>("/api/wardriver/filters", { ouis, macs });
      toast(`Saved ${result.saved} filters`, "success");
    } catch {
      toast("Error saving filters", "error");
    }
  };

  const clearFilters = async () => {
    try {
      await postEmpty("/api/wardriver/clear-filters");
      setOuis("");
      setMacs("");
      setFiltersLoaded(false);
      toast("Filters cleared", "success");
    } catch {
      toast("Error clearing filters", "error");
    }
  };

  const clearDevices = async () => {
    try {
      await postEmpty("/api/wardriver/clear-devices");
      toast("Watchlist history cleared", "success");
    } catch {
      toast("Error clearing devices", "error");
    }
  };

  const devices = devicesResp?.devices ?? [];

  return (
    <div>
      <Card title="WARDRIVING">
        <div class="mb-1.5 flex gap-1.5">
          <StatCard value={status?.wifi ?? 0} label="WIFI" />
          <StatCard value={status?.ble ?? 0} label="BLE" />
          <StatCard value={status?.unique ?? 0} label="UNIQUE" />
        </div>

        <div class="mb-1.5 flex items-center gap-2 text-xs">
          <span class={status?.gps_fresh ? "text-green-400" : "text-text-secondary"}>
            GPS: {status?.gps_fresh ? "FRESH" : "STALE"}
          </span>
          {status?.has_csv && (
            <span class="text-text-secondary">CSV: {formatSize(status?.csv_size ?? 0)}</span>
          )}
        </div>

        <div class="mb-1.5 flex gap-1.5">
          {!status?.active ? (
            <Button onClick={startSession}>START SESSION</Button>
          ) : (
            <Button variant="danger" onClick={stopSession}>
              STOP SESSION
            </Button>
          )}
        </div>

        {status?.has_csv && (
          <div class="mb-1.5 flex gap-1.5">
            <a
              href="/api/wardriver/download"
              class="mb-1.5 block w-full cursor-pointer rounded-md bg-bg-card px-2 py-2 text-center font-mono text-xs font-bold text-accent border border-border-dim hover:border-accent/30 transition-colors"
            >
              DOWNLOAD CSV
            </a>
          </div>
        )}

        <Button variant="secondary" onClick={clearData}>
          CLEAR DATA
        </Button>

        {recent && recent.length > 0 && (
          <div class="mt-1.5">
            <h4 class="mb-1 text-[11px] font-bold text-text-secondary">RECENT SIGHTINGS</h4>
            {recent.map((s, i) => (
              <DeviceCard key={`${s.mac}-${i}`} mac={s.mac} subtitle={s.ssid || undefined}>
                <Tag>{s.type}</Tag>
                <Tag>{s.rssi} dBm</Tag>
                {s.channel > 0 && <Tag>Ch {s.channel}</Tag>}
                <Tag>{s.ago}s ago</Tag>
              </DeviceCard>
            ))}
          </div>
        )}

        {(!recent || recent.length === 0) && <EmptyState message="No sightings yet" />}
      </Card>

      <Card title="WATCHLIST">
        <TextArea
          label="OUI Prefixes (one per line)"
          value={ouis}
          onInput={setOuis}
          placeholder="AA:BB:CC"
        />
        <TextArea
          label="Full MACs (one per line)"
          value={macs}
          onInput={setMacs}
          placeholder="AA:BB:CC:DD:EE:FF"
        />
        <div class="mb-1.5 flex gap-1.5">
          <Button onClick={saveFilters}>SAVE FILTERS</Button>
        </div>
        <Button variant="secondary" onClick={clearFilters}>
          CLEAR FILTERS
        </Button>

        {devices.length > 0 && (
          <>
            <h4 class="mb-1 mt-2 text-[11px] font-bold text-text-secondary">DETECTED DEVICES</h4>
            {devices.map((d) => (
              <DeviceCard key={d.mac} mac={d.mac} subtitle={d.alias ? `(${d.alias})` : undefined}>
                <Tag>{d.rssi} dBm</Tag>
                <Tag>{d.filter}</Tag>
              </DeviceCard>
            ))}
            <Button variant="secondary" onClick={clearDevices}>
              CLEAR WATCHLIST HISTORY
            </Button>
          </>
        )}

        {devices.length === 0 && <EmptyState message="No watchlist detections" />}
      </Card>
    </div>
  );
}
