<script lang="ts">
	import { onMount } from 'svelte';
	import { goto } from '$app/navigation';
	import { pads } from '$lib/pads.svelte';
	import SensorBar from '$lib/components/SensorBar.svelte';
	import SensorEditor from '$lib/components/SensorEditor.svelte';
	import CalibrationModal from '$lib/components/CalibrationModal.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import * as Select from '$lib/components/ui/select';
	import SettingsIcon from '@lucide/svelte/icons/settings';
	import SaveIcon from '@lucide/svelte/icons/save';
	import PlusIcon from '@lucide/svelte/icons/plus';
	import ScaleIcon from '@lucide/svelte/icons/scale';

	let showServers = $state(false);
	let showCalibration = $state(false);
	let endpointsText = $state('');
	let editingIndex = $state<number | null>(null);

	onMount(() => {
		endpointsText = pads.endpoints.join('\n');
	});

	// Auto-pick the first device once one shows up and nothing is chosen yet.
	$effect(() => {
		if (!pads.active && pads.devices.length > 0) {
			const d = pads.devices[0];
			pads.selectDevice(d.endpoint, d.index);
		}
	});

	// Clear optimistic thresholds the pad has caught up to, on every snapshot.
	$effect(() => {
		void pads.activeConn?.snapshot;
		pads.reconcile();
	});

	const devices = $derived(pads.devices);
	const activeLabel = $derived(
		devices.find((d) => d.key === pads.activeKey)?.label ?? 'Select a pad'
	);
	const snap = $derived(pads.activeSnapshot);
	const sensors = $derived(pads.mappedSensors);
	const editing = $derived(sensors.find((m) => m.index === editingIndex));
	const anyOpen = $derived(pads.conns.some((c) => c.status === 'open'));

	const profiles = $derived(pads.compatibleProfiles);
	const loadedProfile = $derived(pads.loadedProfile);
	const modified = $derived(pads.isModified);

	function onSelect(key: string | undefined) {
		if (!key) return;
		const at = key.lastIndexOf('#');
		pads.selectDevice(key.slice(0, at), Number(key.slice(at + 1)));
	}

	function onSelectProfile(id: string | undefined) {
		if (id) pads.loadProfile(id);
	}

	function newProfile() {
		const name = window.prompt('New profile name');
		if (name && name.trim()) pads.saveProfileAs(name.trim());
	}

	function saveProfile() {
		if (loadedProfile) pads.overwriteProfile(loadedProfile.id);
		else newProfile();
	}

	function applyServers() {
		pads.setEndpoints(endpointsText.split('\n'));
		showServers = false;
	}
</script>

<div class="mx-auto flex h-[100dvh] max-w-4xl flex-col gap-6 overflow-hidden p-4 sm:p-6">
	<header class="flex flex-wrap items-center gap-2">
		<Select.Root type="single" value={pads.activeKey} onValueChange={onSelect}>
			<Select.Trigger class="w-56">{activeLabel}</Select.Trigger>
			<Select.Content>
				{#each devices as d (d.key)}
					<Select.Item value={d.key} label={d.label}>{d.label}</Select.Item>
				{/each}
				{#if devices.length === 0}
					<div class="text-muted-foreground px-2 py-1.5 text-sm">No pads found</div>
				{/if}
			</Select.Content>
		</Select.Root>

		<Button variant="outline" size="icon" aria-label="Configure" onclick={() => goto('/config')}>
			<SettingsIcon />
		</Button>

		{#if snap}
			<Select.Root type="single" value={loadedProfile?.id ?? ''} onValueChange={onSelectProfile}>
				<Select.Trigger class="w-56">
					{loadedProfile?.name ?? 'No profile'}
				</Select.Trigger>
				<Select.Content>
					{#each profiles as p (p.id)}
						<Select.Item value={p.id} label={p.name}>{p.name}</Select.Item>
					{/each}
					{#if profiles.length === 0}
						<div class="text-muted-foreground px-2 py-1.5 text-sm">
							No profiles for {snap.sensors.length} sensors
						</div>
					{/if}
				</Select.Content>
			</Select.Root>

			{#if loadedProfile && modified}
				<Badge variant="secondary">modified</Badge>
			{/if}

			<Button size="icon" aria-label="Save" onclick={saveProfile}>
				<SaveIcon />
			</Button>
			<Button variant="outline" size="icon" aria-label="New profile" onclick={newProfile}>
				<PlusIcon />
			</Button>
			<Button variant="outline" size="sm" onclick={() => goto('/profiles')}>Manage</Button>

			{#if sensors.length > 0}
				<Button
					variant="outline"
					size="icon"
					aria-label="Calibrate sensors"
					onclick={() => (showCalibration = true)}
				>
					<ScaleIcon />
				</Button>
			{/if}
		{/if}

		<Button variant="outline" size="sm" class="ml-auto" onclick={() => (showServers = !showServers)}
			>Servers</Button
		>
	</header>

	<!-- per-endpoint connection status -->
	<div class="flex flex-wrap gap-2">
		{#each pads.conns as c (c.endpoint)}
			<Badge variant="outline" class="gap-1.5 font-normal">
				<span
					class="size-2 rounded-full {c.status === 'open'
						? 'bg-green-500'
						: c.status === 'connecting'
							? 'bg-yellow-500'
							: 'bg-red-500'}"
				></span>
				{c.endpoint.replace(/^wss?:\/\//, '')}
			</Badge>
		{/each}
	</div>

	{#if showServers}
		<div class="border-border bg-card flex flex-col gap-2 rounded-lg border p-4">
			<p class="text-muted-foreground text-sm">One server URL per line.</p>
			<textarea
				bind:value={endpointsText}
				rows={Math.max(2, pads.endpoints.length)}
				class="border-input bg-background focus-visible:ring-ring w-full rounded-md border p-2 font-mono text-sm focus-visible:ring-2 focus-visible:outline-none"
			></textarea>
			<div class="flex justify-end">
				<Button size="sm" onclick={applyServers}>Apply</Button>
			</div>
		</div>
	{/if}

	<main class="flex min-h-0 flex-1 items-center justify-center">
		{#if snap}
			{#if sensors.length > 0}
				<div class="flex h-full max-h-[32rem] w-full flex-col items-center gap-2 py-2">
					<div class="grid h-full min-h-0 w-full grid-flow-col auto-cols-fr gap-1 sm:gap-2">
						{#each sensors as m (m.index)}
							<SensorBar
								sensor={m.sensor}
								onthreshold={(v) => pads.setThreshold(m.index, v)}
								onedit={() => (editingIndex = m.index)}
							/>
						{/each}
					</div>
					<p class="text-muted-foreground shrink-0 text-sm">
						{snap.name || 'Pad'} · {snap.pollingRate} Hz
					</p>
				</div>
			{:else}
				<p class="text-muted-foreground">This pad has no mapped sensors.</p>
			{/if}
		{:else if pads.switching}
			<p class="text-muted-foreground">Switching pad…</p>
		{:else if pads.active}
			<p class="text-muted-foreground">Waiting for pad data…</p>
		{:else if devices.length > 0}
			<p class="text-muted-foreground">Select a pad to begin.</p>
		{:else if anyOpen}
			<p class="text-muted-foreground">Connected — no pad detected. Plug in a pad.</p>
		{:else}
			<p class="text-muted-foreground">Connecting to server… check it's running (Servers).</p>
		{/if}
	</main>
</div>

{#if editing}
	<SensorEditor
		sensor={editing.sensor}
		index={editing.index}
		onthreshold={(v) => pads.setThreshold(editing.index, v)}
		onrelease={(v) => pads.setReleaseThreshold(editing.index, v)}
		ongain={(b) => pads.setGain(editing.index, b)}
		onclose={() => (editingIndex = null)}
	/>
{/if}

{#if showCalibration}
	<CalibrationModal
		onapply={(pct, rel) => pads.calibrateThresholds(pct, rel)}
		onclose={() => (showCalibration = false)}
	/>
{/if}
