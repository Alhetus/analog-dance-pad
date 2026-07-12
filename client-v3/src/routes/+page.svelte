<script lang="ts">
	import { onMount } from 'svelte';
	import { pads } from '$lib/pads.svelte';
	import SensorBar from '$lib/components/SensorBar.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import * as Select from '$lib/components/ui/select';

	let showServers = $state(false);
	let endpointsText = $state('');

	onMount(() => {
		pads.start();
		endpointsText = pads.endpoints.join('\n');
		return () => pads.stop();
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
	const anyOpen = $derived(pads.conns.some((c) => c.status === 'open'));

	function onSelect(key: string | undefined) {
		if (!key) return;
		const at = key.lastIndexOf('#');
		pads.selectDevice(key.slice(0, at), Number(key.slice(at + 1)));
	}

	function applyServers() {
		pads.setEndpoints(endpointsText.split('\n'));
		showServers = false;
	}
</script>

<div class="mx-auto flex min-h-screen max-w-4xl flex-col gap-6 p-4 sm:p-6">
	<header class="flex flex-wrap items-center gap-3">
		<h1 class="text-primary mr-auto text-xl font-bold tracking-tight">ADP</h1>

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

		<Button variant="outline" size="sm" onclick={() => (showServers = !showServers)}>Servers</Button
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

	<main class="flex flex-1 items-center justify-center">
		{#if snap}
			{#if sensors.length > 0}
				<div class="flex w-full flex-col items-center gap-4">
					<div class="flex items-end gap-2 overflow-x-auto sm:gap-3">
						{#each sensors as m (m.index)}
							<SensorBar sensor={m.sensor} onthreshold={(v) => pads.setThreshold(m.index, v)} />
						{/each}
					</div>
					<p class="text-muted-foreground text-sm">
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
