import { useEffect, useState } from "preact/hooks";
import type { APConfig, GPSData, Module, SystemStatus } from "../../api/client";
import { postEmpty, postForm } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { Button } from "../shared/button";
import { Card } from "../shared/card";
import { LoadingState } from "../shared/spinner";
import { StatCard } from "../shared/stat-card";
import { TextInput } from "../shared/text-input";
import { useToast } from "../shared/toast";
import { Toggle } from "../shared/toggle";

export function SystemPage() {
  const { toast } = useToast();
  const { data: status, loading: statusLoading } = usePoll<SystemStatus>("/api/status", 5000);
  const {
    data: modules,
    refresh: refreshMods,
    loading: modsLoading,
  } = usePoll<Module[]>("/api/modules", 5000);
  const { data: gps } = usePoll<GPSData>("/api/gps", 5000);
  const { data: ap } = usePoll<APConfig>("/api/ap", 30000);

  const [ssid, setSsid] = useState("");
  const [pass, setPass] = useState("");
  const [ssidError, setSsidError] = useState("");
  const [passError, setPassError] = useState("");
  const [saving, setSaving] = useState(false);
  const [toggling, setToggling] = useState<string | null>(null);

  // Sync AP ssid when loaded
  useEffect(() => {
    if (ap && ssid === "" && ap.ssid) {
      setSsid(ap.ssid);
    }
  }, [ap, ssid]);

  if (statusLoading && modsLoading) return <LoadingState />;

  const formatUptime = (s: number) => {
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    return h > 0 ? `${h}h${String(m % 60).padStart(2, "0")}m` : `${m}m`;
  };

  const toggleModule = async (name: string, enabled: boolean) => {
    setToggling(name);
    try {
      await postForm("/api/modules/toggle", {
        name,
        enabled: String(enabled),
      });
      refreshMods();
    } catch {
      toast("Failed to toggle module", "error");
    } finally {
      setToggling(null);
    }
  };

  const toggleBuzzer = async () => {
    if (!status) return;
    try {
      await postForm("/api/buzzer", { on: String(!status.buzzer) });
    } catch {
      toast("Failed to toggle buzzer", "error");
    }
  };

  const validateAndSaveAP = async () => {
    let valid = true;
    if (!ssid || ssid.length < 1 || ssid.length > 32) {
      setSsidError("SSID must be 1-32 characters");
      valid = false;
    } else {
      setSsidError("");
    }
    if (pass.length > 0 && (pass.length < 8 || pass.length > 63)) {
      setPassError("Password must be 8-63 chars or empty");
      valid = false;
    } else {
      setPassError("");
    }
    if (!valid) return;

    setSaving(true);
    try {
      await postForm("/api/ap", { ssid, pass });
      toast("Saved! Rebooting...", "success");
    } catch {
      toast("Failed to save AP settings", "error");
    } finally {
      setSaving(false);
    }
  };

  const restart = async () => {
    if (!confirm("Restart device?")) return;
    try {
      await postEmpty("/api/reset");
      toast("Restarting...", "info");
    } catch {
      toast("Failed to restart", "error");
    }
  };

  const gpsText = (() => {
    if (!gps) return "Loading...";
    if (gps.hw_fix) return `HW GPS: ${gps.sats} sats, ${gps.lat.toFixed(6)},${gps.lon.toFixed(6)}`;
    if (gps.fresh) return `Phone GPS: ${gps.lat.toFixed(6)},${gps.lon.toFixed(6)}`;
    if (gps.hw_detected) return `HW GPS: ${gps.sats} sats (no fix)`;
    return "No GPS";
  })();

  const isHTTPS = typeof location !== "undefined" && location.protocol === "https:";

  return (
    <div>
      <div class="mb-1.5 flex gap-1.5">
        <StatCard value={status ? Math.round(status.heap / 1024) : "-"} label="HEAP KB" />
        <StatCard value={status ? Math.round(status.psram / 1024) : "-"} label="PSRAM KB" />
        <StatCard value={status ? formatUptime(status.uptime) : "-"} label="UPTIME" />
      </div>

      <Card title="MODULES">
        {modules?.map((m) => (
          <Toggle
            key={m.name}
            label={m.name.toUpperCase()}
            enabled={m.enabled}
            onToggle={() => toggleModule(m.name, !m.enabled)}
          />
        ))}
        {toggling && <div class="text-[10px] text-text-secondary">Toggling {toggling}...</div>}
      </Card>

      <Card title="BUZZER">
        <Toggle label="Buzzer Audio" enabled={status?.buzzer ?? false} onToggle={toggleBuzzer} />
      </Card>

      <Card title="GPS">
        <div class="text-xs text-text-bright">{gpsText}</div>
        {!isHTTPS && !gps?.hw_fix && (
          <div class="mt-1 text-[10px] text-text-muted">Switch to HTTPS for phone GPS</div>
        )}
      </Card>

      <Card title="HTTPS">
        <div class="text-xs text-text-bright">
          {isHTTPS ? "Secure connection active" : "Using HTTP (insecure)"}
        </div>
        {!isHTTPS && (
          <div class="mt-1">
            <a href="/ca.cer" class="text-[11px] text-accent underline">
              Download CA Certificate
            </a>
          </div>
        )}
      </Card>

      <Card title="AP SETTINGS">
        <TextInput label="SSID" value={ssid} onInput={setSsid} maxLength={32} />
        {ssidError && <div class="mb-1 text-[10px] text-danger-bright">{ssidError}</div>}
        <TextInput label="Password" value={pass} onInput={setPass} maxLength={63} />
        {passError && <div class="mb-1 text-[10px] text-danger-bright">{passError}</div>}
        <Button onClick={validateAndSaveAP} loading={saving}>
          SAVE & REBOOT
        </Button>
      </Card>

      <Button variant="danger" onClick={restart}>
        RESTART DEVICE
      </Button>
    </div>
  );
}
