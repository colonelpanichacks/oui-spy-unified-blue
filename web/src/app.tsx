import { useEffect, useRef, useState } from "preact/hooks";
import type { GPSData } from "./api/client";
import { fetchJSON } from "./api/client";
import { DetectorPage } from "./components/detector/detector-page";
import { FlockyouPage } from "./components/flockyou/flockyou-page";
import { FoxhunterPage } from "./components/foxhunter/foxhunter-page";
import { Header } from "./components/layout/header";
import { PageShell } from "./components/layout/page-shell";
import { TabBar } from "./components/layout/tab-bar";
import { HttpsBanner } from "./components/shared/https-banner";
import { ToastProvider } from "./components/shared/toast";
import { SkyspyPage } from "./components/skyspy/skyspy-page";
import { SystemPage } from "./components/system/system-page";
import { WardriverPage } from "./components/wardriver/wardriver-page";
import { usePoll } from "./hooks/use-poll";

const TABS = ["SYSTEM", "DETECT", "FOX", "FLOCK", "SKY", "DRIVE"] as const;

/**
 * Auto-request browser geolocation on HTTPS and send to device.
 * Runs at the app level so all modules benefit (flockyou, wardriver, etc.)
 * regardless of which tab is active.
 */
function usePhoneGPS() {
  const { data: gps } = usePoll<GPSData>("/api/gps", 10000);
  const geoWatchRef = useRef<number | null>(null);

  useEffect(() => {
    const isHTTPS = typeof location !== "undefined" && location.protocol === "https:";
    if (!isHTTPS || !navigator.geolocation) return;

    // Don't request phone GPS if hardware GPS has a fix
    if (gps?.hw_fix) return;

    const watchId = navigator.geolocation.watchPosition(
      (pos) => {
        const { latitude, longitude, accuracy } = pos.coords;
        fetchJSON(`/api/gps/set?lat=${latitude}&lon=${longitude}&acc=${accuracy}`).catch(() => {});
      },
      () => {},
      { enableHighAccuracy: true, timeout: 10000, maximumAge: 5000 },
    );
    geoWatchRef.current = watchId;

    return () => {
      navigator.geolocation.clearWatch(watchId);
      geoWatchRef.current = null;
    };
  }, [gps?.hw_fix]);
}

export function App() {
  const [activeTab, setActiveTab] = useState(0);
  usePhoneGPS();

  return (
    <ToastProvider>
      <Header />
      <HttpsBanner />
      <TabBar tabs={TABS} active={activeTab} onSelect={setActiveTab} />
      <PageShell>
        {activeTab === 0 && <SystemPage />}
        {activeTab === 1 && <DetectorPage />}
        {activeTab === 2 && <FoxhunterPage />}
        {activeTab === 3 && <FlockyouPage />}
        {activeTab === 4 && <SkyspyPage />}
        {activeTab === 5 && <WardriverPage />}
      </PageShell>
    </ToastProvider>
  );
}
