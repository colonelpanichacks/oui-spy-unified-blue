import type { FlockyouDetection, FlockyouStats, Module } from "../../api/client";
import { fetchJSON } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { Button } from "../shared/button";
import { DeviceCard, Tag } from "../shared/device-card";
import { EmptyState } from "../shared/empty-state";
import { ModuleBadge } from "../shared/module-badge";
import { LoadingState } from "../shared/spinner";
import { StatCard } from "../shared/stat-card";
import { useToast } from "../shared/toast";

export function FlockyouPage() {
  const { toast } = useToast();
  const {
    data: detections,
    refresh,
    loading,
  } = usePoll<FlockyouDetection[]>("/api/flockyou/detections", 2500);
  const { data: stats } = usePoll<FlockyouStats>("/api/flockyou/stats", 2500);
  const { data: modules } = usePoll<Module[]>("/api/modules", 10000);

  const moduleEnabled = modules?.find((m) => m.name === "flockyou")?.enabled ?? true;

  if (loading) return <LoadingState />;

  const sorted = detections ? [...detections].sort((a, b) => b.last - a.last) : [];
  const ravenCount = detections?.filter((d) => d.raven).length ?? 0;

  const gpsDisplay = (() => {
    if (!stats) return { text: "-", color: undefined };
    if (stats.gps_src === "hw") return { text: `HW ${stats.gps_sats}sat`, color: "#00ff41" };
    if (stats.gps_src === "phone") return { text: "PHONE", color: "#00ff41" };
    return { text: "OFF", color: "#660000" };
  })();

  const clearAll = async () => {
    if (!confirm("Clear all?")) return;
    try {
      await fetchJSON("/api/flockyou/clear");
      refresh();
      toast("Detections cleared", "success");
    } catch {
      toast("Failed to clear", "error");
    }
  };

  const isHTTPS = typeof location !== "undefined" && location.protocol === "https:";

  return (
    <div>
      <ModuleBadge moduleName="flockyou" enabled={moduleEnabled} />

      <div class="mb-1.5 flex gap-1.5">
        <StatCard value={detections?.length ?? 0} label="DETECTED" />
        <StatCard value={ravenCount} label="RAVEN" />
        <StatCard
          value={gpsDisplay.text}
          label="GPS"
          color={gpsDisplay.color}
          style={{ fontSize: "12px" }}
        />
      </div>

      {sorted.length === 0 ? (
        <EmptyState message="Scanning..." hint="Scanning for surveillance devices via BLE..." />
      ) : (
        sorted.map((d) => (
          <DeviceCard key={d.mac} mac={d.mac} subtitle={d.name || undefined}>
            <Tag>{d.rssi}</Tag>
            <Tag>{d.method}</Tag>
            <Tag>x{d.count}</Tag>
            {d.raven && <Tag color="#ff4444">RAVEN {d.fw || ""}</Tag>}
            {d.gps && <Tag color="#00ff41">GPS</Tag>}
          </DeviceCard>
        ))
      )}

      {!isHTTPS && !stats?.gps_hw_detected && (
        <div class="my-1 text-[10px] text-text-muted">Switch to HTTPS for phone GPS</div>
      )}

      <hr class="my-2 border-border-dim" />
      <Button
        onClick={() => {
          location.href = "/api/flockyou/export/json";
        }}
      >
        EXPORT JSON
      </Button>
      <Button
        onClick={() => {
          location.href = "/api/flockyou/export/csv";
        }}
      >
        EXPORT CSV
      </Button>
      <Button
        variant="secondary"
        onClick={() => {
          location.href = "/api/flockyou/export/kml";
        }}
      >
        EXPORT KML
      </Button>
      <Button variant="danger" onClick={clearAll}>
        CLEAR ALL
      </Button>
    </div>
  );
}
