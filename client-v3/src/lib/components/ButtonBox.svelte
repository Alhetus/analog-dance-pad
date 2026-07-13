<script lang="ts">
	import type { ButtonGroup } from '$lib/pads.svelte';
	import SensorBar from './SensorBar.svelte';

	interface Props {
		group: ButtonGroup;
		releaseEnabled: boolean;
		onthreshold: (index: number, value: number) => void;
		onedit: (index: number) => void;
	}

	let { group, releaseEnabled, onthreshold, onedit }: Props = $props();

	// The button is active when any of its sensors is pressed (pad-authoritative).
	const active = $derived(group.sensors.some((m) => m.sensor.pressed));
</script>

<div
	class="bg-card flex h-full min-h-0 flex-col gap-2 rounded-xl p-2 ring-1 transition-shadow {active
		? 'ring-primary shadow-[0_0_18px_2px_var(--color-primary)]'
		: 'ring-foreground/10'}"
>
	<span
		class="text-secondary-foreground shrink-0 text-center text-sm leading-none font-semibold tabular-nums"
	>
		{group.button}
	</span>

	<div class="grid min-h-0 flex-1 grid-flow-col auto-cols-fr gap-1 sm:gap-2">
		{#each group.sensors as m (m.index)}
			<SensorBar
				sensor={m.sensor}
				{releaseEnabled}
				onthreshold={(v) => onthreshold(m.index, v)}
				onedit={() => onedit(m.index)}
			/>
		{/each}
	</div>
</div>
