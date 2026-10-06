import type { components } from './api.generated';

export type View = components['schemas']['View'];
export type ChartView = components['schemas']['ChartView'];
export type Candle = components['schemas']['Candle'];
export type DepthPoint = components['schemas']['DepthPoint'];
export type Action = components['schemas']['ActionResult'];
export type Order = components['schemas']['OrderView'];
export type Scenario = components['schemas']['ScenarioInfo'];
export type Sessions = components['schemas']['SessionList'];
export type Command =
  | components['schemas']['NewCommand']
  | components['schemas']['CancelCommand']
  | components['schemas']['ReplaceCommand'];

export async function request<T>(path: string, body?: unknown, method = 'POST'): Promise<T> {
  const response = await fetch(`/api/v1${path}`, {
    method: body === undefined ? 'GET' : method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) {
    const value = await response.json().catch(() => ({}));
    throw new Error(
      typeof value.detail === 'string'
        ? value.detail
        : 'The request is invalid. Check integer fields and try again.',
    );
  }
  return response.json() as Promise<T>;
}
