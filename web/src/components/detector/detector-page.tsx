import { useEffect, useState } from "preact/hooks";
import type { DetectorDevice, DetectorFilter, Module } from "../../api/client";
import { postForm } from "../../api/client";
import { usePoll } from "../../hooks/use-poll";
import { Button } from "../shared/button";
import { Card } from "../shared/card";
import { DeviceCard, Tag } from "../shared/device-card";
import { EmptyState } from "../shared/empty-state";
import { ModuleBadge } from "../shared/module-badge";
import { LoadingState } from "../shared/spinner";
import { TextArea } from "../shared/text-input";
import { useToast } from "../shared/toast";

interface DevicesResponse {
  devices: DetectorDevice[];
}

const OUI_RE = /^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}$/;
const MAC_RE =
  /^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}$/;

function countInvalid(text: string, re: RegExp): number {
  if (!text.trim()) return 0;
  return text
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !re.test(l)).length;
}

export function DetectorPage() {
  const { toast } = useToast();
  const { data: filters, loading } = usePoll<DetectorFilter[]>("/api/detector/filters", 10000);
  const { data: devicesResp } = usePoll<DevicesResponse>("/api/detector/devices", 3000);
  const { data: modules } = usePoll<Module[]>("/api/modules", 10000);

  const [ouis, setOuis] = useState("");
  const [macs, setMacs] = useState("");
  const [filtersLoaded, setFiltersLoaded] = useState(false);
  const [saving, setSaving] = useState(false);

  const moduleEnabled = modules?.find((m) => m.name === "detector")?.enabled ?? true;

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

  if (loading) return <LoadingState />;

  const invalidOuis = countInvalid(ouis, OUI_RE);
  const invalidMacs = countInvalid(macs, MAC_RE);

  const saveFilters = async () => {
    setSaving(true);
    try {
      const result = await postForm<{ saved: number }>("/api/detector/filters", { ouis, macs });
      toast(`Saved ${result.saved} filters`, "success");
    } catch {
      toast("Error saving filters", "error");
    } finally {
      setSaving(false);
    }
  };

  const devices = devicesResp?.devices ?? [];

  return (
    <div>
      <ModuleBadge moduleName="detector" enabled={moduleEnabled} />

      <Card title="FILTERS">
        <TextArea
          label="OUI Prefixes (one per line)"
          value={ouis}
          onInput={setOuis}
          placeholder="AA:BB:CC"
        />
        {invalidOuis > 0 && (
          <div class="mb-1 text-[10px] text-danger-bright">
            {invalidOuis} invalid OUI {invalidOuis === 1 ? "entry" : "entries"} will be skipped
          </div>
        )}
        <TextArea
          label="Full MACs (one per line)"
          value={macs}
          onInput={setMacs}
          placeholder="AA:BB:CC:DD:EE:FF"
        />
        {invalidMacs > 0 && (
          <div class="mb-1 text-[10px] text-danger-bright">
            {invalidMacs} invalid MAC {invalidMacs === 1 ? "entry" : "entries"} will be skipped
          </div>
        )}
        <Button onClick={saveFilters} loading={saving}>
          SAVE FILTERS
        </Button>
      </Card>

      <Card title="DETECTED DEVICES">
        {devices.length === 0 ? (
          <EmptyState
            message="No detections yet"
            hint="Enable Detector module and add MAC filters to start"
          />
        ) : (
          devices.map((d) => (
            <DeviceCard key={d.mac} mac={d.mac} subtitle={d.alias ? `(${d.alias})` : undefined}>
              <Tag>{d.rssi} dBm</Tag>
              <Tag>{d.filter}</Tag>
            </DeviceCard>
          ))
        )}
      </Card>
    </div>
  );
}
