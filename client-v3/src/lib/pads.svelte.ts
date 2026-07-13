import { browser } from '$app/environment';
import serversConfig from './servers.config.json';

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
export type SensorPatch = Partial<
	Pick<Sensor, 'threshold' | 'releaseThreshold' | 'resistorValue' | 'button'>
>;

/** LED lighting config, in the same shape a saved profile uses. */
export interface LightRule {
	fadeOn: boolean;
	fadeOff: boolean;
	onColor: string;
	offColor: string;
	onFadeColor: string;
	offFadeColor: string;
}

export interface LedMapping {
	lightRuleIndex: number;
	sensorIndex: number;
	ledIndexBegin: number;
	ledIndexEnd: number;
}

export interface Snapshot {
	msgType: 1;
	name: string;
	pollingRate: number;
	selectedIndex: number;
	releaseThreshold: number; // legacy global release ratio, 0.01..1 (unused; see releaseMode)
	releaseMode: number; // 0 none, 2 per-sensor (1 = legacy global, treated as per-sensor)
	numButtons: number; // mappable buttons this pad exposes
	featureDigipot: boolean; // pad supports per-sensor gain
	sensors: Sensor[];
}

/** Lights message (msgType 4) — sent on change / periodically, not per snapshot. */
export interface Lights {
	lightRules: LightRule[];
	ledMappings: LedMapping[];
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

/** Mapped sensors sharing one button, grouped for display. */
export interface ButtonGroup {
	button: number; // 1-based button number
	sensors: MappedSensor[]; // sensors mapped to this button, in snapshot-index order
}

export interface DeviceOption {
	endpoint: string;
	index: number;
	id: string;
	name: string;
	key: string; // `${endpoint}#${index}` — the Select value
	label: string;
}

/** A stored profile (msgType 3). Captures thresholds+release, gain and lights. */
export interface Profile {
	id: string; // immutable filename stem
	name: string;
	description?: string;
	sensorCount: number; // only loadable onto a pad with this many sensors
	savedAt?: number; // unix seconds
	sensors: Array<Pick<Sensor, 'threshold' | 'releaseThreshold' | 'resistorValue'>>;
	releaseThreshold?: number;
	lightRules?: LightRule[];
	ledMappings?: LedMapping[];
}

// Reconcile tolerance for float thresholds. Must sit between the device's
// quantization noise (thresholds are stored /850 ~= 0.0012) and the smallest
// deliberate edit (the +/- fine-tune buttons nudge by 1% = 0.01). If EPS were
// >= that step, reconcile() would treat the pre-update snapshot as already
// converged and delete a single-percent nudge before the device applied it.
const EPS = 0.004;
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
	profiles: Profile[] = $state([]);
	lights: Lights | null = $state(null); // null until the first lights message arrives
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
			this.send({ listProfiles: true }); // get the profile list right away
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
		} else if (msg.msgType === 3) {
			this.profiles = (m as { profiles: Profile[] }).profiles;
		} else if (msg.msgType === 4) {
			this.lights = m as Lights;
		}
	}

	#onDown() {
		if (this.#closed) return;
		this.status = 'offline';
		this.snapshot = null;
		this.deviceList = [];
		this.profiles = [];
		this.lights = null;
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

const ACTIVE_KEY = 'adp-active';
const LOADED_KEY = 'adp-loaded'; // { [padKey]: profileId } — last profile loaded onto each pad
/** Server list is baked in at build time from servers.config.json — not user-editable. */
const CONFIGURED_ENDPOINTS = serversConfig.servers;

/** Order-independent deep equality via key-sorted JSON (both sides come from the
 * server's nlohmann serialization, so this is exact for lights comparison). */
const canon = (v: unknown): string =>
	JSON.stringify(v, (_, val) =>
		val && typeof val === 'object' && !Array.isArray(val)
			? Object.fromEntries(
					Object.keys(val as object)
						.sort()
						.map((k) => [k, (val as Record<string, unknown>)[k]])
				)
			: val
	);

/** Same slug rule as the server (Profiles::SlugifyId), to predict a new id. */
const slugify = (name: string) => {
	const s = name
		.toLowerCase()
		.replace(/[^a-z0-9]+/g, '-')
		.replace(/^-+|-+$/g, '');
	return s || 'profile';
};

class PadsStore {
	endpoints: string[] = $state([...CONFIGURED_ENDPOINTS]);
	conns: Conn[] = $state([]);
	active: ActiveRef | null = $state(null);
	/** Optimistic per-sensor edits for the active device, keyed by sensor index. */
	pending: Record<number, SensorPatch> = $state({});
	/** Last profile loaded onto each pad, keyed by `${endpoint}#${index}`. */
	loaded: Record<string, string> = $state({});
	now = $state(0); // ticking clock so staleness stays reactive

	#clock: ReturnType<typeof setInterval> | null = null;
	#sendTimer: ReturnType<typeof setTimeout> | null = null;

	/** Call once from the browser (onMount). Safe to call more than once. */
	start() {
		if (!browser || this.#clock) return;
		try {
			const a = localStorage.getItem(ACTIVE_KEY);
			if (a) this.active = JSON.parse(a);
			const l = localStorage.getItem(LOADED_KEY);
			if (l) this.loaded = JSON.parse(l);
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

	/**
	 * Whether per-sensor release thresholds are in effect. False only in "None"
	 * mode (release == press threshold). Any non-zero mode (including legacy global)
	 * counts as per-sensor. Defaults to true until a snapshot arrives.
	 */
	get releaseEnabled(): boolean {
		return (this.activeSnapshot?.releaseMode ?? 2) !== 0;
	}

	/** True once a device is chosen but the server hasn't switched to it yet. */
	get switching(): boolean {
		const c = this.#activeConn();
		if (!this.active || !c) return false;
		return c.status === 'open' && c.selectedIndex !== this.active.index;
	}

	/** Mapped sensors of the active device, with optimistic thresholds overlaid. */
	get mappedSensors(): MappedSensor[] {
		return this.allSensors.filter((m) => m.sensor.button > 0);
	}

	/** Mapped sensors grouped by button, sorted ascending; empty buttons skipped. */
	get buttonGroups(): ButtonGroup[] {
		const byButton = new Map<number, MappedSensor[]>();
		for (const m of this.mappedSensors) {
			let group = byButton.get(m.sensor.button);
			if (!group) byButton.set(m.sensor.button, (group = []));
			group.push(m);
		}
		return [...byButton.entries()]
			.sort((a, b) => a[0] - b[0])
			.map(([button, sensors]) => ({ button, sensors }));
	}

	/** Every sensor of the active device (mapped or not), with optimistic edits overlaid. */
	get allSensors(): MappedSensor[] {
		const snap = this.activeSnapshot;
		if (!snap) return [];
		return snap.sensors.map((s, index) => {
			const p = this.pending[index];
			return { index, sensor: p === undefined ? s : { ...s, ...p } };
		});
	}

	// ---- Profiles ----------------------------------------------------------

	/** All stored profiles, deduped by id (every server shares the same folder). */
	get profiles(): Profile[] {
		const byId = new Map<string, Profile>();
		for (const c of this.conns) for (const p of c.profiles) if (!byId.has(p.id)) byId.set(p.id, p);
		return [...byId.values()].sort((a, b) => a.name.localeCompare(b.name));
	}

	/** Sensor count of the active pad (profiles must match this to be loadable). */
	get activeSensorCount(): number | null {
		return this.activeSnapshot?.sensors.length ?? null;
	}

	/** Profiles that can be loaded onto the active pad (matching sensor count). */
	get compatibleProfiles(): Profile[] {
		const n = this.activeSensorCount;
		return n === null ? [] : this.profiles.filter((p) => p.sensorCount === n);
	}

	/** The profile last loaded onto the active pad, if it still exists. */
	get loadedProfile(): Profile | null {
		const id = this.active ? this.loaded[this.activeKey] : undefined;
		return id ? (this.profiles.find((p) => p.id === id) ?? null) : null;
	}

	/** True if the active pad's live settings have drifted from the loaded profile. */
	get isModified(): boolean {
		const p = this.loadedProfile;
		const snap = this.activeSnapshot;
		if (!p || !snap) return false;
		const n = Math.min(p.sensors.length, snap.sensors.length);
		for (let i = 0; i < n; i++) {
			const a = p.sensors[i];
			const b = snap.sensors[i];
			if (Math.abs((a.threshold ?? 0) - b.threshold) > EPS) return true;
			if (Math.abs((a.releaseThreshold ?? 0) - b.releaseThreshold) > EPS) return true;
			if (a.resistorValue !== undefined && a.resistorValue !== b.resistorValue) return true;
		}
		// Lights arrive in their own (gated) message; only compare once we've got
		// them, so we don't flag drift before the first lights message lands.
		const lights = this.activeConn?.lights;
		if (lights) {
			if (canon(p.lightRules ?? []) !== canon(lights.lightRules)) return true;
			if (canon(p.ledMappings ?? []) !== canon(lights.ledMappings)) return true;
		}
		return false;
	}

	/** The active server if any, else any open connection (for file-only ops). */
	#anyConn(): Conn | undefined {
		return this.activeConn ?? this.conns.find((c) => c.status === 'open');
	}

	#persistLoaded() {
		if (browser) localStorage.setItem(LOADED_KEY, JSON.stringify(this.loaded));
	}

	#markLoaded(id: string | null) {
		if (!this.active) return;
		const next = { ...this.loaded };
		if (id === null) delete next[this.activeKey];
		else next[this.activeKey] = id;
		this.loaded = next;
		this.#persistLoaded();
	}

	/** Apply a stored profile to the active pad (server validates sensor count). */
	loadProfile(id: string) {
		this.activeConn?.send({ loadProfile: id });
		this.#markLoaded(id);
	}

	/** Capture the active pad's live settings as a new named profile. */
	saveProfileAs(name: string) {
		this.activeConn?.send({ saveProfile: { name } });
		// ponytail: optimistically assume the predicted slug id. A duplicate name
		// gets a suffixed id server-side and would mislabel until the list arrives.
		this.#markLoaded(slugify(name));
	}

	/** Re-capture the active pad's live settings into an existing profile. */
	overwriteProfile(id: string) {
		this.activeConn?.send({ saveProfile: { id } });
		this.#markLoaded(id);
	}

	/** Edit a profile's name/description (no re-capture). */
	updateProfile(id: string, name: string, description: string) {
		this.#anyConn()?.send({ updateProfile: { id, name, description } });
	}

	deleteProfile(id: string) {
		this.#anyConn()?.send({ deleteProfile: id });
		// Drop it from any pad's last-loaded pointer.
		const next = { ...this.loaded };
		let changed = false;
		for (const k of Object.keys(next))
			if (next[k] === id) {
				delete next[k];
				changed = true;
			}
		if (changed) {
			this.loaded = next;
			this.#persistLoaded();
		}
	}

	/** Select which device to view/control; tells that server to stream it. */
	selectDevice(endpoint: string, index: number) {
		this.active = { endpoint, index };
		this.pending = {};
		if (browser) localStorage.setItem(ACTIVE_KEY, JSON.stringify(this.active));
		const conn = this.conns.find((c) => c.endpoint === endpoint);
		if (conn) conn.lights = null; // stale for the new pad until its lights message arrives
		conn?.send({ selectDevice: index });
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
		const v = clamp01(value);
		const patch: SensorPatch = { threshold: v };
		// The device keeps release at a fixed ratio of threshold (global mode);
		// overlay it in lockstep so the release marker tracks the drag instead of
		// lagging until the pad echoes the recomputed value back.
		const s = this.#activeConn()?.snapshot?.sensors[sensorIndex];
		if (s && s.threshold > 0)
			patch.releaseThreshold = clamp01(v * (s.releaseThreshold / s.threshold));
		this.#patch(sensorIndex, patch);
	}

	setReleaseThreshold(sensorIndex: number, value: number) {
		// Release can never exceed the press threshold (hysteresis would break); the
		// server enforces this too. Cap against the most current threshold we have.
		const s = this.#activeConn()?.snapshot?.sensors[sensorIndex];
		const cap = this.pending[sensorIndex]?.threshold ?? s?.threshold ?? 1;
		this.#patch(sensorIndex, { releaseThreshold: clamp01(Math.min(value, cap)) });
	}

	/** Set the raw digipot gain byte (0..255). */
	setGain(sensorIndex: number, byte: number) {
		this.#patch(sensorIndex, { resistorValue: clampByte(byte) });
	}

	/** Map a sensor to a button (0 = unmapped/disabled, else 1-based). */
	setButton(sensorIndex: number, button: number) {
		this.#patch(sensorIndex, { button: Math.max(0, Math.round(button)) });
	}

	// ---- Device-level config (not per-sensor; sent directly, snapshot echoes back) ----

	setName(name: string) {
		this.activeConn?.send({ name });
	}

	/** Release mode: 0 none, 2 per-sensor. */
	setReleaseMode(mode: number) {
		this.activeConn?.send({ releaseMode: mode });
	}

	/** Recalibrate one sensor's baseline. */
	calibrate(sensorIndex: number) {
		this.activeConn?.send({ calibrateSensor: sensorIndex });
	}

	/**
	 * Set threshold = live value + offset (percent points) across mapped sensors.
	 * Re-reads each sensor's current live value as the baseline on every call, so
	 * the pad should be at rest when this runs. When `overwriteRelease` is true the
	 * release threshold is set equal to the new threshold; otherwise only the
	 * threshold is written. All patches flush together in one `{sensors:[...]}` send.
	 */
	calibrateThresholds(offsetPct: number, overwriteRelease: boolean) {
		const snap = this.activeSnapshot;
		if (!snap) return;
		const offset = offsetPct / 100;
		snap.sensors.forEach((s, i) => {
			if (s.button <= 0) return; // mapped sensors only
			const threshold = clamp01(s.value + offset);
			if (overwriteRelease)
				this.#patch(i, { threshold, releaseThreshold: threshold }); // release == press
			else this.#patch(i, { threshold }); // threshold only
		});
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
			if (patch.button !== undefined && s.button === patch.button) delete patch.button;
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
