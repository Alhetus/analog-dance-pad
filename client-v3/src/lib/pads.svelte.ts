import { browser } from '$app/environment';
import serversConfig from './servers.config.json';

// ---- Protocol types (see adp-tool Device.cpp / Players.cpp) ----

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

/** msgType 1, one per pad per tick. */
export interface Snapshot {
	msgType: 1;
	pad: string; // HID path: how messages address this pad
	name: string;
	pollingRate: number;
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

/** A connected pad (msgType 2). */
export interface DeviceInfo {
	id: string; // HID path
	name: string; // stable identity: profiles are stored per pad name
	sensorCount: number;
}

/** A player from the shared LocalProfiles folder (msgType 3). */
export interface Player {
	name: string;
	avatar: boolean;
	profiles: Record<string, number>; // pad name -> stored profile count
}

/** Server-owned selection on one pad (msgType 5). */
export interface PadState {
	name: string;
	player?: string;
	profileId?: string;
	profileName?: string;
	recent?: string[];
}

/** A stored profile (msgType 6). Captures thresholds+release, gain and lights. */
export interface Profile {
	id: string; // filename stem, unique per player+pad
	name: string;
	description?: string;
	sensorCount: number; // only loadable onto a pad with this many sensors
	savedAt?: number; // unix seconds
	sensors: Array<Pick<Sensor, 'threshold' | 'releaseThreshold' | 'resistorValue'>>;
	releaseThreshold?: number;
	lightRules?: LightRule[];
	ledMappings?: LedMapping[];
}

export interface Avatar {
	mime: string; // '' when the player has none
	url: string; // data: URL
}

/** Mapped sensors grouped per button, plus each sensor's index in the snapshot. */
export interface MappedSensor {
	index: number;
	sensor: Sensor;
}

export interface ButtonGroup {
	button: number; // 1-based button number
	sensors: MappedSensor[]; // sensors mapped to this button, in snapshot-index order
}

export interface ServerConfig {
	name: string;
	url: string;
}

/** The 4-step flow, then tuning. */
export type Step = 'server' | 'pad' | 'player' | 'profile' | 'tune';

export const MAX_PROFILES = 5; // per player per pad, enforced by the server too

// Reconcile tolerance for float thresholds. Must sit between the device's
// quantization noise (thresholds are stored /850 ~= 0.0012) and the smallest
// deliberate edit (the +/- fine-tune buttons nudge by 1% = 0.01). If EPS were
// >= that step, reconcile() would treat the pre-update snapshot as already
// converged and delete a single-percent nudge before the device applied it.
const EPS = 0.004;
const SEND_THROTTLE_MS = 40;
// An optimistic edit the pad hasn't echoed back by then was refused or clamped
// by the server; drop it so the UI shows the pad's real value again.
const PENDING_TTL_MS = 1000;
const STALE_MS = 1000; // no snapshot for this long while open => pad considered gone
const MAX_BACKOFF_MS = 5000;

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clampByte = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));

/** One WebSocket connection to a single adp-tool server. */
class Conn {
	readonly endpoint: string;
	readonly name: string;
	status: 'connecting' | 'open' | 'offline' = $state('connecting');
	deviceList: DeviceInfo[] = $state([]);
	/** Replaced (never mutated) per message: raw state skips deep-proxying 60Hz data. */
	snapshots: Record<string, Snapshot> = $state.raw({});
	/** Arrival time per pad; not reactive, read alongside `snapshots` and the ticking clock. */
	snapshotAt: Record<string, number> = {};
	lights: Record<string, Lights> = $state({});
	players: Player[] = $state([]);
	/** False while the server can't reach LocalProfiles; `players` is then the last known list. */
	localProfilesOk = $state(true);
	padStates: Record<string, PadState> = $state({});
	/** player -> pad name -> profiles, filled by listProfiles replies. */
	profiles: Record<string, Record<string, Profile[]>> = $state({});
	/** player -> avatar; null while requested. */
	avatars: Record<string, Avatar | null> = $state({});

	#ws: WebSocket | null = null;
	#backoff = 500;
	#reconnectTimer: ReturnType<typeof setTimeout> | null = null;
	#closed = false;
	/** Pads this client wants snapshots for: '*' (server default), a pad path, or '' for none. */
	#subscription = '*';

	constructor(server: ServerConfig) {
		this.endpoint = server.url;
		this.name = server.name;
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
		// Events of a socket we've already replaced must not touch the new one.
		ws.addEventListener('open', () => {
			if (ws !== this.#ws) return;
			this.status = 'open';
			this.#backoff = 500;
			// The server sends its current pads, players and lights on connect.
			if (this.#subscription !== '*') this.send({ subscribe: this.#subscription });
		});
		ws.addEventListener('message', (ev) => ws === this.#ws && this.#onMessage(ev.data));
		ws.addEventListener('close', () => ws === this.#ws && this.#onDown());
		ws.addEventListener('error', () => ws === this.#ws && this.#onDown());
	}

	#onMessage(data: string) {
		let m: unknown;
		try {
			m = JSON.parse(data);
		} catch {
			return;
		}
		if (typeof m !== 'object' || m === null) return;
		const msg = m as { msgType?: number; pad?: string };
		if (msg.msgType === 1) {
			const s = m as Snapshot;
			this.snapshotAt[s.pad] = Date.now();
			this.snapshots = { ...this.snapshots, [s.pad]: s };
		} else if (msg.msgType === 2) {
			this.deviceList = (m as { devices: DeviceInfo[] }).devices;
		} else if (msg.msgType === 3) {
			const pl = m as { players: Player[]; localProfiles?: boolean };
			this.players = pl.players;
			this.localProfilesOk = pl.localProfiles ?? true;
		} else if (msg.msgType === 4) {
			if (msg.pad) this.lights[msg.pad] = m as Lights;
		} else if (msg.msgType === 5) {
			this.padStates = (m as { pads: Record<string, PadState> }).pads;
		} else if (msg.msgType === 6) {
			const r = m as { player: string; pads: Record<string, Profile[]> };
			this.profiles[r.player] = r.pads;
		} else if (msg.msgType === 7) {
			const a = m as { name: string; mime: string; data: string };
			this.avatars[a.name] = { mime: a.mime, url: a.mime ? `data:${a.mime};base64,${a.data}` : '' };
		}
	}

	#onDown() {
		if (this.#closed) return;
		this.status = 'offline';
		this.snapshots = {};
		this.deviceList = [];
		this.lights = {};
		this.padStates = {};
		this.avatars = {}; // requests made while down were dropped; ask again on reconnect
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

	/** Which pads to stream snapshots for; resent on every reconnect. */
	subscribe(sub: string) {
		if (sub === this.#subscription) return;
		this.#subscription = sub;
		this.send({ subscribe: sub });
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

const SERVER_KEY = 'adp-server';
/** Server list is baked in at build time from servers.config.json — not user-editable. */
const SERVERS: ServerConfig[] = serversConfig.servers;

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

class PadsStore {
	readonly servers = SERVERS;
	conns: Conn[] = $state([]);

	// ---- Selection: server -> pad -> player -> profile ----
	step: Step = $state('server');
	server: string | null = $state(null); // endpoint url, remembered per device
	pad: string | null = $state(null); // HID path on that server
	player: string | null = $state(null);
	/** Set when a player is picked; the profile step auto-loads a lone profile once. */
	autoLoad = $state(false);

	/** Optimistic per-sensor edits for the selected pad, keyed by sensor index. */
	pending: Record<number, SensorPatch> = $state({});
	now = $state(0); // ticking clock so staleness stays reactive

	#clock: ReturnType<typeof setInterval> | null = null;
	#sendTimer: ReturnType<typeof setTimeout> | null = null;
	#stopEffects: (() => void) | null = null;
	/** When each pending sensor patch was last edited (see PENDING_TTL_MS). */
	#pendingAt: Record<number, number> = {};

	/** Call once from the browser (onMount). Safe to call more than once. */
	start() {
		if (!browser || this.#clock) return;
		try {
			const s = localStorage.getItem(SERVER_KEY);
			if (s && SERVERS.some((c) => c.url === s)) {
				this.server = s;
				this.step = 'pad';
			}
		} catch {
			/* storage unavailable */
		}
		this.now = Date.now();
		this.#clock = setInterval(() => (this.now = Date.now()), 250);
		this.conns = SERVERS.map((s) => new Conn(s));
		// Stream only what's on screen: every pad of the selected server while
		// picking one, then just the picked pad; nothing from other servers
		// (they are only asked for profiles to copy).
		this.#stopEffects = $effect.root(() => {
			$effect(() => {
				for (const c of this.conns)
					c.subscribe(
						c.endpoint !== this.server ? '' : this.step === 'pad' || !this.pad ? '*' : this.pad
					);
			});
		});
	}

	stop() {
		if (this.#clock) clearInterval(this.#clock);
		this.#clock = null;
		this.#stopEffects?.();
		this.#stopEffects = null;
		for (const c of this.conns) c.close();
		this.conns = [];
	}

	// ---- Navigation ----

	selectServer(url: string) {
		this.server = url;
		this.pad = null;
		this.player = null;
		this.pending = {};
		this.step = 'pad';
		try {
			localStorage.setItem(SERVER_KEY, url);
		} catch {
			/* storage unavailable */
		}
	}

	selectPad(path: string) {
		this.pad = path;
		this.player = null;
		this.pending = {};
		this.step = 'player';
		this.conn?.send({ rescanPlayers: true }); // pick up just-created players
	}

	selectPlayer(name: string) {
		this.player = name;
		this.autoLoad = true;
		this.step = 'profile';
		this.#requestProfiles(name);
	}

	goto(step: Step) {
		if (step === 'profile' && this.player) this.#requestProfiles(this.player);
		this.autoLoad = false;
		this.step = step;
	}

	/** Every server keeps only its own pads' profiles: ask them all (copy sources). */
	#requestProfiles(player: string) {
		for (const c of this.conns) c.send({ listProfiles: { player } });
	}

	// ---- Derived views ----

	get conn(): Conn | undefined {
		return this.conns.find((c) => c.endpoint === this.server);
	}

	get serverName(): string {
		return this.conn?.name ?? '';
	}

	/** The selected server's pads. */
	get devices(): DeviceInfo[] {
		return this.conn?.deviceList ?? [];
	}

	get device(): DeviceInfo | undefined {
		return this.devices.find((d) => d.id === this.pad);
	}

	/** Pad names used more than once on the selected server, where profiles are keyed by pad name. */
	get duplicatePadNames(): string[] {
		const names = this.devices.map((d) => d.name);
		return names.filter((n, i) => names.indexOf(n) !== i);
	}

	/** A pad's live snapshot on the selected server, or null if stale/missing. */
	snapshotOf(path: string): Snapshot | null {
		const c = this.conn;
		if (!c || c.status !== 'open') return null;
		const s = c.snapshots[path];
		return s && this.now - (c.snapshotAt[path] ?? 0) < STALE_MS ? s : null;
	}

	padStateOf(path: string): PadState | undefined {
		return this.conn?.padStates[path];
	}

	get activeSnapshot(): Snapshot | null {
		return this.pad ? this.snapshotOf(this.pad) : null;
	}

	get padState(): PadState | undefined {
		return this.pad ? this.padStateOf(this.pad) : undefined;
	}

	get localProfilesOk(): boolean {
		return this.conn?.localProfilesOk ?? true;
	}

	get players(): Player[] {
		return this.conn?.players ?? [];
	}

	/** Profiles of `player` on pad `padName` (once listProfiles has answered). */
	profilesOf(player: string, padName: string): Profile[] | undefined {
		const byPad = this.conn?.profiles[player];
		return byPad ? (byPad[padName] ?? []) : undefined;
	}

	/** The selected player's profiles on the selected pad; undefined while loading. */
	get playerProfiles(): Profile[] | undefined {
		const d = this.device;
		return this.player && d ? this.profilesOf(this.player, d.name) : undefined;
	}

	/** The selected player's compatible profiles on *other* pads, on any server (copy sources). */
	get copySources(): Array<{ server: string; pad: string; profile: Profile }> {
		const d = this.device;
		const player = this.player;
		if (!d || !player) return [];
		return this.conns.flatMap((c) =>
			Object.entries(c.profiles[player] ?? {}).flatMap(([pad, list]) =>
				c === this.conn && pad === d.name
					? []
					: list
							.filter((p) => p.sensorCount === d.sensorCount)
							.map((profile) => ({ server: c.name, pad, profile }))
			)
		);
	}

	/** Whether per-sensor release thresholds are in effect (false only in "None" mode). */
	get releaseEnabled(): boolean {
		return (this.activeSnapshot?.releaseMode ?? 2) !== 0;
	}

	/** Every sensor of the selected pad (mapped or not), with optimistic edits overlaid. */
	get allSensors(): MappedSensor[] {
		const snap = this.activeSnapshot;
		if (!snap) return [];
		return snap.sensors.map((s, index) => {
			const p = this.pending[index];
			return { index, sensor: p === undefined ? s : { ...s, ...p } };
		});
	}

	get mappedSensors(): MappedSensor[] {
		return this.allSensors.filter((m) => m.sensor.button > 0);
	}

	/** Mapped sensors grouped by button, sorted ascending; empty buttons skipped. */
	get buttonGroups(): ButtonGroup[] {
		return groupByButton(this.mappedSensors);
	}

	/** The profile currently on the selected pad, if it belongs to the selected player. */
	get loadedProfile(): Profile | null {
		const st = this.padState;
		if (!st?.profileId || st.player !== this.player) return null;
		return this.playerProfiles?.find((p) => p.id === st.profileId) ?? null;
	}

	/** True if the pad's live settings have drifted from the loaded profile. */
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
		const lights = this.pad ? this.conn?.lights[this.pad] : undefined;
		if (lights) {
			if (canon(p.lightRules ?? []) !== canon(lights.lightRules)) return true;
			if (canon(p.ledMappings ?? []) !== canon(lights.ledMappings)) return true;
		}
		return false;
	}

	/** A player's avatar, once fetched (see requestAvatar). undefined = none / not yet. */
	avatar(player: Player): Avatar | undefined {
		return this.conn?.avatars[player.name] ?? undefined;
	}

	/** Fetches a player's avatar once per connection. Writes state: call from an effect, not a derived. */
	requestAvatar(player: Player) {
		const c = this.conn;
		if (!c || c.status !== 'open' || !player.avatar || player.name in c.avatars) return;
		c.avatars[player.name] = null;
		c.send({ getAvatar: player.name });
	}

	// ---- Profile actions (selected pad + player) ----

	#sendPad(msg: Record<string, unknown>) {
		if (this.pad) this.conn?.send({ pad: this.pad, ...msg });
	}

	loadProfile(id: string) {
		this.#sendPad({ loadProfile: { player: this.player, id } });
		this.pending = {};
	}

	saveProfileAs(name: string) {
		this.#sendPad({ saveProfile: { player: this.player, name } });
	}

	overwriteProfile(id: string) {
		this.#sendPad({ saveProfile: { player: this.player, id } });
	}

	/**
	 * Store a copy of `profile` (read from this or any other server) on the
	 * selected pad as `name`. Purely client-side across servers: the profile
	 * travels in the message, the target server validates and stores it.
	 */
	copyProfile(profile: Profile, name: string) {
		this.#sendPad({ importProfile: { player: this.player, name, profile } });
	}

	updateProfile(id: string, name: string, description: string) {
		this.#sendPad({ updateProfile: { player: this.player, id, name, description } });
	}

	deleteProfile(id: string) {
		this.#sendPad({ deleteProfile: { player: this.player, id } });
	}

	// ---- Sensor edits (optimistic, throttled) ----

	#patch(sensorIndex: number, patch: SensorPatch) {
		this.#pendingAt[sensorIndex] = Date.now();
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
		// Keep release at its current ratio of threshold so the release marker
		// tracks the drag instead of lagging until the pad echoes it back.
		const s = this.activeSnapshot?.sensors[sensorIndex];
		if (s && s.threshold > 0)
			patch.releaseThreshold = clamp01(v * (s.releaseThreshold / s.threshold));
		this.#patch(sensorIndex, patch);
	}

	setReleaseThreshold(sensorIndex: number, value: number) {
		// Release can never exceed the press threshold (hysteresis would break); the
		// server enforces this too. Cap against the most current threshold we have.
		const s = this.activeSnapshot?.sensors[sensorIndex];
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

	// ---- Device-level config (sent directly, snapshot echoes back) ----

	/** Renames the pad; the server moves its stored profiles along. */
	setName(name: string) {
		this.#sendPad({ name });
	}

	/** Release mode: 0 none, 2 per-sensor. */
	setReleaseMode(mode: number) {
		this.#sendPad({ releaseMode: mode });
	}

	/** Recalibrate one sensor's baseline. */
	calibrate(sensorIndex: number) {
		this.#sendPad({ calibrateSensor: sensorIndex });
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
		const snap = this.activeSnapshot;
		if (!snap) return;
		// Positional array: {} for untouched sensors, the patch for pending ones.
		const sensors = snap.sensors.map((_, i) => this.pending[i] ?? {});
		this.#sendPad({ sensors });
	}

	/** Clear optimistic values the server has caught up to. Call each snapshot. */
	reconcile() {
		const snap = this.activeSnapshot;
		if (!snap) return;
		let changed = false;
		const next = { ...this.pending };
		const now = Date.now();
		for (const k of Object.keys(next)) {
			const i = Number(k);
			const s = snap.sensors[i];
			if (!s || now - (this.#pendingAt[i] ?? 0) > PENDING_TTL_MS) {
				delete next[i];
				changed = true;
				continue;
			}
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

/** Mapped sensors grouped by button, sorted ascending; empty buttons skipped. */
export function groupByButton(mapped: MappedSensor[]): ButtonGroup[] {
	// Stable sort keeps snapshot-index order within a button.
	const groups: ButtonGroup[] = [];
	for (const m of [...mapped].sort((a, b) => a.sensor.button - b.sensor.button)) {
		const last = groups.at(-1);
		if (last?.button === m.sensor.button) last.sensors.push(m);
		else groups.push({ button: m.sensor.button, sensors: [m] });
	}
	return groups;
}

export const pads = new PadsStore();
