import { browser } from '$app/environment';

// ---- Protocol types (see adp-tool Device::SnapshotToJson / DeviceListToJson) ----

export interface Sensor {
	threshold: number; // normalized 0..1
	releaseThreshold: number; // normalized 0..1
	value: number; // normalized 0..1
	resistorValue: number; // raw digipot byte 0..255 (NOT normalized)
	button: number; // 0 = unmapped, else 1-based
	pressed: boolean; // pad-authoritative activation
}

/** Fields a client may write back per sensor (positional in the `sensors` message). */
export type SensorPatch = Partial<Pick<Sensor, 'threshold' | 'releaseThreshold' | 'resistorValue'>>;

export interface Snapshot {
	msgType: 1;
	name: string;
	pollingRate: number;
	selectedIndex: number;
	sensors: Sensor[];
}

export interface DeviceInfo {
	index: number;
	id: string;
	name: string;
}

/** A mapped sensor plus its original index in the snapshot (needed to write back). */
export interface MappedSensor {
	index: number;
	sensor: Sensor;
}

export interface DeviceOption {
	endpoint: string;
	index: number;
	id: string;
	name: string;
	key: string; // `${endpoint}#${index}` — the Select value
	label: string;
}

const EPS = 0.01; // device quantizes thresholds to /850 (~0.0012), so echoes differ slightly
const SEND_THROTTLE_MS = 40;
const STALE_MS = 1000; // no snapshot for this long while open => pad considered gone
const MAX_BACKOFF_MS = 5000;

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clampByte = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));
const hostLabel = (endpoint: string) => endpoint.replace(/^wss?:\/\//, '');

/** One WebSocket connection to a single adp-tool server. */
class Conn {
	readonly endpoint: string;
	status: 'connecting' | 'open' | 'offline' = $state('connecting');
	deviceList: DeviceInfo[] = $state([]);
	selectedIndex = $state(-1);
	snapshot: Snapshot | null = $state(null);
	lastMessageAt = $state(0);

	#ws: WebSocket | null = null;
	#backoff = 500;
	#reconnectTimer: ReturnType<typeof setTimeout> | null = null;
	#closed = false;

	constructor(endpoint: string) {
		this.endpoint = endpoint;
		this.#open();
	}

	#open() {
		this.status = 'connecting';
		let ws: WebSocket;
		try {
			ws = new WebSocket(this.endpoint);
		} catch {
			this.#scheduleReconnect();
			return;
		}
		this.#ws = ws;
		ws.addEventListener('open', () => {
			this.status = 'open';
			this.#backoff = 500;
		});
		ws.addEventListener('message', (ev) => this.#onMessage(ev.data));
		ws.addEventListener('close', () => this.#onDown());
		ws.addEventListener('error', () => this.#onDown());
	}

	#onMessage(data: string) {
		let m: unknown;
		try {
			m = JSON.parse(data);
		} catch {
			return;
		}
		if (typeof m !== 'object' || m === null) return;
		const msg = m as { msgType?: number };
		if (msg.msgType === 2) {
			const dl = m as { devices: DeviceInfo[]; selectedIndex: number };
			this.deviceList = dl.devices;
			this.selectedIndex = dl.selectedIndex;
		} else if (msg.msgType === 1) {
			this.snapshot = m as Snapshot;
			this.selectedIndex = (m as Snapshot).selectedIndex;
			this.lastMessageAt = Date.now();
		}
	}

	#onDown() {
		if (this.#closed) return;
		this.status = 'offline';
		this.snapshot = null;
		this.deviceList = [];
		this.#scheduleReconnect();
	}

	#scheduleReconnect() {
		if (this.#closed || this.#reconnectTimer) return;
		this.#reconnectTimer = setTimeout(() => {
			this.#reconnectTimer = null;
			this.#open();
		}, this.#backoff);
		this.#backoff = Math.min(this.#backoff * 2, MAX_BACKOFF_MS);
	}

	send(obj: unknown) {
		if (this.#ws && this.status === 'open') this.#ws.send(JSON.stringify(obj));
	}

	close() {
		this.#closed = true;
		if (this.#reconnectTimer) clearTimeout(this.#reconnectTimer);
		this.#ws?.close();
	}
}

interface ActiveRef {
	endpoint: string;
	index: number;
}

const ENDPOINTS_KEY = 'adp-endpoints';
const ACTIVE_KEY = 'adp-active';
const DEFAULT_ENDPOINTS = ['ws://127.0.0.1:8008'];

class PadsStore {
	endpoints: string[] = $state([...DEFAULT_ENDPOINTS]);
	conns: Conn[] = $state([]);
	active: ActiveRef | null = $state(null);
	/** Optimistic per-sensor edits for the active device, keyed by sensor index. */
	pending: Record<number, SensorPatch> = $state({});
	now = $state(0); // ticking clock so staleness stays reactive

	#clock: ReturnType<typeof setInterval> | null = null;
	#sendTimer: ReturnType<typeof setTimeout> | null = null;

	/** Call once from the browser (onMount). Safe to call more than once. */
	start() {
		if (!browser || this.#clock) return;
		try {
			const e = localStorage.getItem(ENDPOINTS_KEY);
			if (e) this.endpoints = JSON.parse(e);
			const a = localStorage.getItem(ACTIVE_KEY);
			if (a) this.active = JSON.parse(a);
		} catch {
			/* ignore malformed storage */
		}
		this.now = Date.now();
		this.#clock = setInterval(() => (this.now = Date.now()), 250);
		this.#reconcileConns();
	}

	stop() {
		if (this.#clock) clearInterval(this.#clock);
		this.#clock = null;
		for (const c of this.conns) c.close();
		this.conns = [];
	}

	setEndpoints(list: string[]) {
		this.endpoints = list.map((s) => s.trim()).filter(Boolean);
		if (browser) localStorage.setItem(ENDPOINTS_KEY, JSON.stringify(this.endpoints));
		this.#reconcileConns();
	}

	#reconcileConns() {
		if (!browser) return;
		const wanted = new Set(this.endpoints);
		// Drop connections no longer wanted.
		for (const c of this.conns) if (!wanted.has(c.endpoint)) c.close();
		const kept = this.conns.filter((c) => wanted.has(c.endpoint));
		const have = new Set(kept.map((c) => c.endpoint));
		// Open connections for new endpoints.
		const added = this.endpoints.filter((e) => !have.has(e)).map((e) => new Conn(e));
		this.conns = [...kept, ...added];
	}

	/** All devices across all endpoints, for the dropdown. */
	get devices(): DeviceOption[] {
		const out: DeviceOption[] = [];
		for (const c of this.conns) {
			for (const d of c.deviceList) {
				out.push({
					endpoint: c.endpoint,
					index: d.index,
					id: d.id,
					name: d.name,
					key: `${c.endpoint}#${d.index}`,
					label: `${hostLabel(c.endpoint)} — ${d.name || 'Pad'}`
				});
			}
		}
		return out;
	}

	get activeKey(): string {
		return this.active ? `${this.active.endpoint}#${this.active.index}` : '';
	}

	#activeConn(): Conn | undefined {
		return this.active ? this.conns.find((c) => c.endpoint === this.active!.endpoint) : undefined;
	}

	get activeConn(): Conn | undefined {
		return this.#activeConn();
	}

	#isFresh(c: Conn | undefined): c is Conn {
		return (
			!!c && c.status === 'open' && c.snapshot !== null && this.now - c.lastMessageAt < STALE_MS
		);
	}

	/** The live snapshot for the active device, or null if not (yet) available. */
	get activeSnapshot(): Snapshot | null {
		const c = this.#activeConn();
		if (!this.#isFresh(c)) return null;
		if (!this.active || c.snapshot!.selectedIndex !== this.active.index) return null;
		return c.snapshot;
	}

	/** True once a device is chosen but the server hasn't switched to it yet. */
	get switching(): boolean {
		const c = this.#activeConn();
		if (!this.active || !c) return false;
		return c.status === 'open' && c.selectedIndex !== this.active.index;
	}

	/** Mapped sensors of the active device, with optimistic thresholds overlaid. */
	get mappedSensors(): MappedSensor[] {
		const snap = this.activeSnapshot;
		if (!snap) return [];
		const out: MappedSensor[] = [];
		snap.sensors.forEach((s, index) => {
			if (s.button <= 0) return;
			const p = this.pending[index];
			out.push({ index, sensor: p === undefined ? s : { ...s, ...p } });
		});
		return out;
	}

	/** Select which device to view/control; tells that server to stream it. */
	selectDevice(endpoint: string, index: number) {
		this.active = { endpoint, index };
		this.pending = {};
		if (browser) localStorage.setItem(ACTIVE_KEY, JSON.stringify(this.active));
		this.conns.find((c) => c.endpoint === endpoint)?.send({ selectDevice: index });
	}

	/** Merge an optimistic patch for a sensor and (throttled) push it to the pad. */
	#patch(sensorIndex: number, patch: SensorPatch) {
		this.pending = {
			...this.pending,
			[sensorIndex]: { ...this.pending[sensorIndex], ...patch }
		};
		this.#scheduleSend();
	}

	/** Optimistically set a sensor threshold and (throttled) push it to the pad. */
	setThreshold(sensorIndex: number, value: number) {
		this.#patch(sensorIndex, { threshold: clamp01(value) });
	}

	setReleaseThreshold(sensorIndex: number, value: number) {
		this.#patch(sensorIndex, { releaseThreshold: clamp01(value) });
	}

	/** Set the raw digipot gain byte (0..255). */
	setGain(sensorIndex: number, byte: number) {
		this.#patch(sensorIndex, { resistorValue: clampByte(byte) });
	}

	#scheduleSend() {
		if (this.#sendTimer) return;
		this.#sendTimer = setTimeout(() => {
			this.#sendTimer = null;
			this.#flush();
		}, SEND_THROTTLE_MS);
	}

	#flush() {
		const c = this.#activeConn();
		const snap = c?.snapshot;
		if (!c || !snap) return;
		// Positional array: {} for untouched sensors, the patch for pending ones.
		const sensors = snap.sensors.map((_, i) => this.pending[i] ?? {});
		c.send({ sensors });
	}

	/** Clear optimistic values the server has caught up to. Call each snapshot. */
	reconcile() {
		const snap = this.#activeConn()?.snapshot;
		if (!snap) return;
		let changed = false;
		const next = { ...this.pending };
		for (const k of Object.keys(next)) {
			const i = Number(k);
			const s = snap.sensors[i];
			if (!s) continue;
			const patch: SensorPatch = { ...next[i] };
			const before = Object.keys(patch).length;
			if (patch.threshold !== undefined && Math.abs(s.threshold - patch.threshold) < EPS)
				delete patch.threshold;
			if (
				patch.releaseThreshold !== undefined &&
				Math.abs(s.releaseThreshold - patch.releaseThreshold) < EPS
			)
				delete patch.releaseThreshold;
			if (patch.resistorValue !== undefined && s.resistorValue === patch.resistorValue)
				delete patch.resistorValue;
			const after = Object.keys(patch).length;
			if (after === 0) {
				delete next[i];
				changed = true;
			} else if (after !== before) {
				next[i] = patch;
				changed = true;
			}
		}
		if (changed) this.pending = next;
	}
}

export const pads = new PadsStore();
