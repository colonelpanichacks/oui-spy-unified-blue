import { connected, topic } from "../store/ws-store";
import { usePoll } from "./use-poll";

/**
 * Subscribe to a WS topic with HTTP poll fallback.
 * Polling is disabled while the WS connection is alive.
 */
export function useWsTopic<T>(topicName: string, pollUrl: string, pollInterval: number) {
  const wsData = topic<T>(topicName).value;
  const poll = usePoll<T>(pollUrl, pollInterval, !connected.value);
  return {
    data: wsData ?? poll.data,
    loading: poll.loading && !wsData,
    refresh: poll.refresh,
  };
}
