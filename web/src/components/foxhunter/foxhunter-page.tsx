import { useEffect, useState } from "preact/hooks";
import type { FoxhunterStatus, Module } from "../../api/client";
import { postEmpty, postForm } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { Button } from "../shared/button";
import { Card } from "../shared/card";
import { ModuleBadge } from "../shared/module-badge";
import { ProgressBar } from "../shared/progress-bar";
import { LoadingState } from "../shared/spinner";
import { StatCard } from "../shared/stat-card";
import { TextInput } from "../shared/text-input";
import { useToast } from "../shared/toast";

const MAC_RE =
  /^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}$/;

export function FoxhunterPage() {
  const { toast } = useToast();
  const { data, refresh, loading } = usePoll<FoxhunterStatus>("/api/foxhunter/status", 500);
  const { data: modules } = usePoll<Module[]>("/api/modules", 10000);

  const [mac, setMac] = useState("");
  const [macLoaded, setMacLoaded] = useState(false);
  const [macError, setMacError] = useState("");
  const [setting, setSetting] = useState(false);

  const moduleEnabled = modules?.find((m) => m.name === "foxhunter")?.enabled ?? true;

  useEffect(() => {
    if (data?.target && !macLoaded) {
      setMac(data.target);
      setMacLoaded(true);
    }
  }, [data, macLoaded]);

  if (loading) return <LoadingState />;

  const setTarget = async () => {
    const trimmed = mac.trim();
    if (!trimmed) {
      toast("Enter MAC address", "error");
      return;
    }
    if (!MAC_RE.test(trimmed)) {
      setMacError("Invalid MAC format (XX:XX:XX:XX:XX:XX)");
      return;
    }
    setMacError("");
    setSetting(true);
    try {
      await postForm("/api/foxhunter/target", { mac: trimmed });
      refresh();
    } catch {
      toast("Failed to set target", "error");
    } finally {
      setSetting(false);
    }
  };

  const clearTarget = async () => {
    try {
      await postEmpty("/api/foxhunter/clear");
      setMac("");
      setMacLoaded(false);
      refresh();
      toast("Target cleared", "success");
    } catch {
      toast("Failed to clear target", "error");
    }
  };

  const rssiPercent = data?.detected
    ? Math.min(100, Math.max(0, ((data.rssi + 100) / 70) * 100))
    : 0;

  return (
    <div>
      <ModuleBadge moduleName="foxhunter" enabled={moduleEnabled} />

      <div class="mb-1.5 flex gap-1.5">
        <StatCard value={data?.detected ? data.rssi : "-"} label="RSSI dBm" />
        <StatCard
          value={data?.detected ? "TRACKING" : "SEARCHING"}
          label="STATUS"
          color={data?.detected ? "#00ff41" : "#00aa00"}
        />
      </div>

      <Card title="TARGET MAC">
        <TextInput value={mac} onInput={setMac} placeholder="AA:BB:CC:DD:EE:FF" maxLength={17} />
        {macError && <div class="mb-1 text-[10px] text-danger-bright">{macError}</div>}
        <div class="flex gap-1.5">
          <Button small onClick={setTarget} loading={setting}>
            SET TARGET
          </Button>
          <Button small variant="danger" onClick={clearTarget}>
            CLEAR
          </Button>
        </div>
      </Card>

      <Card title="RSSI METER">
        <ProgressBar percent={rssiPercent} />
        {!data?.target && (
          <div class="mt-1 text-[10px] text-text-muted">
            Set a target MAC address to begin tracking
          </div>
        )}
      </Card>
    </div>
  );
}
