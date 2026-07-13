<script lang="ts">
	import { goto } from '$app/navigation';
	import { pads, type MappedSensor } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import * as Card from '$lib/components/ui/card';
	import * as Select from '$lib/components/ui/select';

	// Auto-pick the first device if none chosen (same as the tuning page).
	$effect(() => {
		if (!pads.active && pads.devices.length > 0) {
			const d = pads.devices[0];
			pads.selectDevice(d.endpoint, d.index);
		}
	});

	// Clear optimistic edits the pad has caught up to, on every snapshot.
	$effect(() => {
		void pads.activeConn?.snapshot;
		pads.reconcile();
	});

	const devices = $derived(pads.devices);
	const activeLabel = $derived(
		devices.find((d) => d.key === pads.activeKey)?.label ?? 'Select a pad'
	);
	const snap = $derived(pads.activeSnapshot);
	const numButtons = $derived(snap?.numButtons ?? 0);
	const buttonOptions = $derived(Array.from({ length: numButtons }, (_, i) => i + 1));

	// Group every sensor by its button; button 0 / out-of-range => Unassigned (disabled).
	const groups = $derived.by(() => {
		const byButton: MappedSensor[][] = Array.from({ length: numButtons + 1 }, () => []);
		const unassigned: MappedSensor[] = [];
		for (const m of pads.allSensors) {
			const b = m.sensor.button;
			if (b >= 1 && b <= numButtons) byButton[b].push(m);
			else unassigned.push(m);
		}
		return { byButton, unassigned };
	});

	// Local drafts so live edits aren't stomped by the incoming snapshot mid-interaction.
	let nameDraft = $state<string | null>(null);
	const nameValue = $derived(nameDraft ?? snap?.name ?? '');

	const str = (e: Event) => (e.currentTarget as HTMLInputElement).value;

	function onSelect(key: string | undefined) {
		if (!key) return;
		const at = key.lastIndexOf('#');
		pads.selectDevice(key.slice(0, at), Number(key.slice(at + 1)));
	}

	function commitName() {
		if (nameDraft !== null && nameDraft !== snap?.name) pads.setName(nameDraft.slice(0, 50));
		nameDraft = null;
	}

	const releaseModes = [
		{ v: '0', label: 'None' },
		{ v: '2', label: 'Per-sensor' }
	];
	// Legacy global (1) and any non-zero mode display as per-sensor.
	const releaseModeValue = $derived(String((snap?.releaseMode ?? 2) === 0 ? 0 : 2));
	const releaseModeLabel = $derived(
		releaseModes.find((m) => m.v === releaseModeValue)?.label ?? 'Per-sensor'
	);
</script>

{#snippet sensorRow(m: MappedSensor)}
	<div class="flex items-center gap-3">
		<span class="text-muted-foreground w-16 shrink-0 text-sm tabular-nums">Sensor {m.index}</span>
		<!-- live value bar -->
		<div class="bg-muted relative h-2 flex-1 overflow-hidden rounded-full">
			<div
				class="absolute inset-y-0 left-0 rounded-full {m.sensor.pressed
					? 'bg-primary'
					: 'bg-muted-foreground/50'}"
				style="width: {Math.round(Math.min(1, Math.max(0, m.sensor.value)) * 100)}%"
			></div>
		</div>
		<Select.Root
			type="single"
			value={String(m.sensor.button)}
			onValueChange={(v) => v && pads.setButton(m.index, Number(v))}
		>
			<Select.Trigger class="w-28 shrink-0"
				>{m.sensor.button > 0 ? `Button ${m.sensor.button}` : 'Off'}</Select.Trigger
			>
			<Select.Content>
				<Select.Item value="0" label="Off">Off</Select.Item>
				{#each buttonOptions as b (b)}
					<Select.Item value={String(b)} label="Button {b}">Button {b}</Select.Item>
				{/each}
			</Select.Content>
		</Select.Root>
	</div>
{/snippet}

<div class="mx-auto flex min-h-[100dvh] max-w-4xl flex-col gap-6 p-4 sm:p-6">
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
		<Button variant="outline" size="sm" class="ml-auto" onclick={() => goto('/')}>
			Back to tuning
		</Button>
	</header>

	{#if snap}
		<!-- Device settings -->
		<Card.Root>
			<Card.Header>
				<Card.Title>Device</Card.Title>
			</Card.Header>
			<Card.Content class="flex flex-col gap-5">
				<div class="flex flex-col gap-1.5">
					<Label for="pad-name">Name</Label>
					<Input
						id="pad-name"
						value={nameValue}
						maxlength={50}
						oninput={(e) => (nameDraft = str(e))}
						onblur={commitName}
						onkeydown={(e) => e.key === 'Enter' && e.currentTarget.blur()}
						class="w-64"
					/>
				</div>

				<div class="flex flex-col gap-1.5">
					<Label>Release threshold</Label>
					<Select.Root
						type="single"
						value={releaseModeValue}
						onValueChange={(v) => v && pads.setReleaseMode(Number(v))}
					>
						<Select.Trigger class="w-40">{releaseModeLabel}</Select.Trigger>
						<Select.Content>
							{#each releaseModes as m (m.v)}
								<Select.Item value={m.v} label={m.label}>{m.label}</Select.Item>
							{/each}
						</Select.Content>
					</Select.Root>
					<p class="text-muted-foreground text-sm">
						<em>None</em> releases as soon as a sensor drops below its threshold.
						<em>Per-sensor</em> lets each sensor release at its own lower threshold (hysteresis).
					</p>
				</div>
			</Card.Content>
		</Card.Root>

		<!-- Button mapping -->
		<div class="flex flex-col gap-2">
			<h2 class="text-lg font-semibold">Button mapping</h2>
			<p class="text-muted-foreground text-sm">
				Assign each sensor to a button. Sensors set to <em>Off</em> are unassigned and don't trigger
				anything.
			</p>
		</div>

		{#if numButtons < 1}
			<p class="text-muted-foreground text-sm">
				This pad reports no buttons. If the server is out of date, rebuild it to expose the button
				count.
			</p>
		{:else}
			<div class="grid gap-4 sm:grid-cols-2">
				{#each buttonOptions as b (b)}
					<Card.Root>
						<Card.Header>
							<Card.Title class="text-base">Button {b}</Card.Title>
						</Card.Header>
						<Card.Content class="flex flex-col gap-2">
							{#each groups.byButton[b] as m (m.index)}
								{@render sensorRow(m)}
							{:else}
								<span class="text-muted-foreground text-sm">No sensors</span>
							{/each}
						</Card.Content>
					</Card.Root>
				{/each}
			</div>

			{#if groups.unassigned.length > 0}
				<Card.Root class="border-dashed">
					<Card.Header>
						<Card.Title class="text-muted-foreground text-base">Unassigned</Card.Title>
					</Card.Header>
					<Card.Content class="flex flex-col gap-2">
						{#each groups.unassigned as m (m.index)}
							{@render sensorRow(m)}
						{/each}
					</Card.Content>
				</Card.Root>
			{/if}
		{/if}
	{:else if pads.switching}
		<p class="text-muted-foreground">Switching pad…</p>
	{:else if pads.active}
		<p class="text-muted-foreground">Waiting for pad data…</p>
	{:else if devices.length > 0}
		<p class="text-muted-foreground">Select a pad to configure.</p>
	{:else}
		<p class="text-muted-foreground">Connecting to server…</p>
	{/if}
</div>
