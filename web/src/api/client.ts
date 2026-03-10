// Typed API client — manual fetch helpers + schema-derived types
import type { components } from "./schema";

// Schema-derived type aliases for component use
export type SystemStatus = components["schemas"]["SystemStatus"];
export type Module = components["schemas"]["Module"];
export type GPSData = components["schemas"]["GPSData"];
export type APConfig = components["schemas"]["APConfig"];
export type DetectorFilter = components["schemas"]["DetectorFilter"];
export type DetectorDevice = components["schemas"]["DetectorDevice"];
export type FoxhunterStatus = components["schemas"]["FoxhunterStatus"];
export type FoxhunterRSSI = components["schemas"]["FoxhunterRSSI"];
export type FlockyouDetection = components["schemas"]["FlockyouDetection"];
export type FlockyouStats = components["schemas"]["FlockyouStats"];
export type SkyspyDrone = components["schemas"]["SkyspyDrone"];
export type SkyspyStatus = components["schemas"]["SkyspyStatus"];
export type WardriverStatus = components["schemas"]["WardriverStatus"];
export type WardriverSighting = components["schemas"]["WardriverSighting"];
export type WardriverConfig = components["schemas"]["WardriverConfig"];
export type WardriverFilter = components["schemas"]["WardriverFilter"];
export type WardriverDevice = components["schemas"]["WardriverDevice"];
export async function fetchJSON<T>(url: string): Promise<T> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`API ${res.status}: ${url}`);
  return res.json();
}

export async function postForm<T>(url: string, params: Record<string, string>): Promise<T> {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams(params).toString(),
  });
  if (!res.ok) throw new Error(`API ${res.status}: ${url}`);
  return res.json();
}

export async function postEmpty<T>(url: string): Promise<T> {
  const res = await fetch(url, { method: "POST" });
  if (!res.ok) throw new Error(`API ${res.status}: ${url}`);
  return res.json();
}
