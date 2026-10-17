<script lang="ts">
	import { pads, type DeviceInfo } from '$lib/pads.svelte';
	import { Badge } from '$lib/components/ui/badge';

	let { device }: { device: DeviceInfo } = $props();

	const snap = $derived(pads.snapshotOf(device.id));
	const padState = $derived(pads.padStateOf(device.id));
	const mapped = $derived(snap?.sensors.filter((s) => s.button > 0) ?? []);
	// Glows while stepped on, so "which pad is this?" is answered by stepping on it.
	const active = $derived(mapped.some((s) => s.pressed));
	const duplicate = $derived(pads.duplicatePadNames.includes(device.name));
</script>

<button
	class="bg-card flex min-h-48 flex-col gap-3 rounded-xl p-5 text-left ring-1 transition-shadow {active
		? 'ring-primary shadow-[0_0_18px_2px_var(--color-primary)]'
		: 'ring-foreground/10'}"
	onclick={() => pads.selectPad(device.id)}
>
	<div class="flex items-center gap-2">
		<span class="text-2xl font-bold">{device.name || 'Pad'}</span>
		{#if duplicate}
			<Badge variant="destructive">duplicate name</Badge>
		{/if}
	</div>

	<div class="flex-1">
		{#if padState?.player}
			<div class="text-lg font-semibold">{padState.player}</div>
			<div class="text-muted-foreground">{padState.profileName || 'no profile'}</div>
		{:else}
			<div class="text-muted-foreground text-lg">Free</div>
		{/if}
	</div>

	<!-- live mini bars: value fill, threshold tick -->
	<div class="flex h-10 items-end gap-1">
		{#each mapped as s, i (i)}
			<div class="bg-muted relative h-full flex-1 overflow-hidden rounded-sm">
				<div
					class="absolute inset-x-0 bottom-0 {s.pressed ? 'bg-primary' : 'bg-muted-foreground/50'}"
					style="height: {Math.round(Math.min(1, Math.max(0, s.value)) * 100)}%"
				></div>
				<div
					class="bg-foreground/60 absolute inset-x-0 h-px"
					style="bottom: {Math.round(s.threshold * 100)}%"
				></div>
			</div>
		{/each}
	</div>
</button>
