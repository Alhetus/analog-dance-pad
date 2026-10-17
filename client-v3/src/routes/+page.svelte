<script lang="ts">
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { pads, type Step } from '$lib/pads.svelte';
	import ButtonBox from '$lib/components/ButtonBox.svelte';
	import SensorEditor from '$lib/components/SensorEditor.svelte';
	import CalibrationModal from '$lib/components/CalibrationModal.svelte';
	import ServerPicker from '$lib/components/ServerPicker.svelte';
	import PadCard from '$lib/components/PadCard.svelte';
	import PlayerPicker from '$lib/components/PlayerPicker.svelte';
	import ProfilePicker from '$lib/components/ProfilePicker.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import SettingsIcon from '@lucide/svelte/icons/settings';
	import SaveIcon from '@lucide/svelte/icons/save';
	import ScaleIcon from '@lucide/svelte/icons/scale';
	import ChevronDownIcon from '@lucide/svelte/icons/chevron-down';

	let showCalibration = $state(false);
	let editingIndex = $state<number | null>(null);

	// The selected pad vanished (unplugged): back to the dashboard. Only once
	// the server's device list is in: right after a (re)connect it is empty.
	$effect(() => {
		if (
			pads.step !== 'server' &&
			pads.step !== 'pad' &&
			pads.conn?.status === 'open' &&
			pads.conn.devicesKnown &&
			!pads.device
		)
			pads.goto('pad');
	});

	const step = $derived(pads.step);
	const snap = $derived(pads.activeSnapshot);
	const sensors = $derived(pads.mappedSensors);
	const groups = $derived(pads.buttonGroups);
	const editing = $derived(sensors.find((m) => m.index === editingIndex));
	const loadedProfile = $derived(pads.loadedProfile);
	const modified = $derived(pads.isModified);
	const order: Step[] = ['server', 'pad', 'player', 'profile', 'tune'];
	const reached = (s: Step) => order.indexOf(step) > order.indexOf(s);

	function save() {
		if (!loadedProfile) pads.goto('profile');
		else if (window.confirm(`Overwrite "${loadedProfile.name}" with the current settings?`))
			pads.overwriteProfile(loadedProfile.id);
	}
</script>

<div class="mx-auto flex h-[100dvh] max-w-5xl flex-col gap-4 overflow-hidden p-4 sm:p-6">
	{#if step !== 'server'}
		<!-- breadcrumb: each chip jumps back to its step -->
		<header class="flex flex-wrap items-center gap-2">
			<Button variant="outline" onclick={() => pads.goto('server')}>
				{pads.serverName}
				<ChevronDownIcon />
			</Button>
			{#if reached('pad')}
				<Button variant="outline" onclick={() => pads.goto('pad')}
					>{pads.device?.name ?? 'Pad'}</Button
				>
			{/if}
			{#if reached('player')}
				<Button variant="outline" onclick={() => pads.goto('player')}>{pads.player}</Button>
			{/if}
			{#if reached('profile')}
				<Button variant="outline" onclick={() => pads.goto('profile')}>
					{loadedProfile?.name ?? 'No profile'}
				</Button>
				{#if loadedProfile && modified}
					<Badge variant="secondary">modified</Badge>
				{/if}
				{#if snap?.saveFailed}
					<Badge variant="destructive" title="The server keeps retrying every 2 seconds"
						>not saved to the pad</Badge
					>
				{:else if snap?.unsaved}
					<Badge variant="outline" title="Edits reach the pad's memory ~2s after the last one"
						>saving…</Badge
					>
				{/if}
			{/if}

			{#if step === 'tune' && snap}
				<div class="ml-auto flex gap-2">
					<Button size="icon" aria-label="Save" onclick={save}><SaveIcon /></Button>
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
					<Button
						variant="outline"
						size="icon"
						aria-label="Configure"
						onclick={() => goto(resolve('/config'))}
					>
						<SettingsIcon />
					</Button>
				</div>
			{/if}
		</header>
	{/if}

	{#if step !== 'server' && pads.conn?.status === 'open' && !pads.localProfilesOk}
		<div class="rounded-lg border border-yellow-500/50 bg-yellow-500/10 px-4 py-2 text-sm">
			Player list is offline (shared LocalProfiles folder not reachable): showing the last known
			players. Retrying automatically.
		</div>
	{/if}

	<main class="flex min-h-0 flex-1 flex-col overflow-y-auto">
		{#if step === 'server'}
			<ServerPicker />
		{:else if step === 'pad'}
			<h1 class="mb-4 text-2xl font-bold">Pick your pad</h1>
			{#if pads.conn?.status !== 'open'}
				<p class="text-muted-foreground">Connecting to {pads.serverName}…</p>
			{:else if pads.devices.length === 0}
				<p class="text-muted-foreground">No pads connected to {pads.serverName}.</p>
			{:else}
				<div class="grid gap-4 sm:grid-cols-2">
					{#each pads.devices as d (d.id)}
						<PadCard device={d} />
					{/each}
				</div>
			{/if}
		{:else if step === 'player'}
			<PlayerPicker />
		{:else if step === 'profile'}
			<ProfilePicker />
		{:else if snap}
			{#if sensors.length > 0}
				<div class="flex h-full max-h-[32rem] w-full flex-col items-center gap-2 py-2">
					<div class="flex h-full min-h-0 w-full gap-2">
						{#each groups as g (g.button)}
							<div class="min-w-0" style="flex: {g.sensors.length} 1 0">
								<ButtonBox
									group={g}
									releaseEnabled={pads.releaseEnabled}
									onthreshold={(i, v) => pads.setThreshold(i, v)}
									onedit={(i) => (editingIndex = i)}
								/>
							</div>
						{/each}
					</div>
					<p class="text-muted-foreground shrink-0 text-sm">
						{snap.name || 'Pad'} · {snap.pollingRate} Hz
					</p>
				</div>
			{:else}
				<p class="text-muted-foreground">This pad has no mapped sensors.</p>
			{/if}
		{:else}
			<p class="text-muted-foreground">Waiting for pad data…</p>
		{/if}
	</main>
</div>

{#if editing}
	<SensorEditor
		sensor={editing.sensor}
		index={editing.index}
		releaseEnabled={pads.releaseEnabled}
		gainEnabled={snap?.featureDigipot ?? false}
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
