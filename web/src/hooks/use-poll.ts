import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import { fetchJSON } from "../api/client";

interface UsePollResult<T> {
  data: T | null;
  error: Error | null;
  loading: boolean;
  connectionLost: boolean;
  refresh: () => void;
}

const MAX_CONSECUTIVE_ERRORS = 5;

export function usePoll<T>(url: string, intervalMs: number, enabled = true): UsePollResult<T> {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const [loading, setLoading] = useState(true);
  const errorCountRef = useRef(0);
  const [connectionLost, setConnectionLost] = useState(false);
  const timerRef = useRef<ReturnType<typeof setInterval>>(
    0 as unknown as ReturnType<typeof setInterval>,
  );

  const doFetch = useCallback(() => {
    fetchJSON<T>(url)
      .then((d) => {
        setData(d);
        setError(null);
        setLoading(false);
        errorCountRef.current = 0;
        setConnectionLost(false);
      })
      .catch((e) => {
        setError(e);
        setLoading(false);
        errorCountRef.current++;
        if (errorCountRef.current >= MAX_CONSECUTIVE_ERRORS) {
          setConnectionLost(true);
        }
      });
  }, [url]);

  useEffect(() => {
    if (!enabled) {
      clearInterval(timerRef.current);
      return;
    }

    doFetch();
    timerRef.current = setInterval(doFetch, intervalMs);

    const onVisibility = () => {
      clearInterval(timerRef.current);
      if (!document.hidden) {
        doFetch();
        timerRef.current = setInterval(doFetch, intervalMs);
      }
    };

    document.addEventListener("visibilitychange", onVisibility);

    return () => {
      clearInterval(timerRef.current);
      document.removeEventListener("visibilitychange", onVisibility);
    };
  }, [intervalMs, doFetch, enabled]);

  return { data, error, loading, connectionLost, refresh: doFetch };
}
