import type { ComponentChildren } from "preact";
import { createContext } from "preact";
import { useCallback, useContext, useRef, useState } from "preact/hooks";

type ToastType = "success" | "error" | "info";

interface Toast {
  id: number;
  message: string;
  type: ToastType;
}

interface ToastContextValue {
  toast: (message: string, type?: ToastType) => void;
}

const ToastContext = createContext<ToastContextValue>({ toast: () => {} });

export function useToast() {
  return useContext(ToastContext);
}

export function ToastProvider({ children }: { children: ComponentChildren }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const nextId = useRef(0);

  const toast = useCallback((message: string, type: ToastType = "info") => {
    const id = ++nextId.current;
    setToasts((prev) => [...prev, { id, message, type }]);
    setTimeout(() => {
      setToasts((prev) => prev.filter((t) => t.id !== id));
    }, 3000);
  }, []);

  const colors: Record<ToastType, string> = {
    success: "border-accent bg-accent/10 text-accent",
    error: "border-danger-bright bg-danger/10 text-danger-bright",
    info: "border-accent-dim bg-accent/5 text-text-secondary",
  };

  return (
    <ToastContext.Provider value={{ toast }}>
      {children}
      <div class="fixed bottom-4 left-1/2 z-50 flex -translate-x-1/2 flex-col gap-2">
        {toasts.map((t) => (
          <div
            key={t.id}
            class={`animate-fade-in rounded-md border px-4 py-2 font-mono text-xs shadow-lg backdrop-blur ${colors[t.type]}`}
          >
            {t.message}
          </div>
        ))}
      </div>
    </ToastContext.Provider>
  );
}
